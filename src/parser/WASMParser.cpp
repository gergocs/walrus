/*
 * Copyright (c) 2022-present Samsung Electronics Co., Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "Walrus.h"

#include "parser/WASMParser.h"
#include "interpreter/ByteCode.h"
#include "runtime/Store.h"
#include "runtime/Module.h"

#include "wabt/walrus/binary-reader-walrus.h"

namespace wabt {

enum class WASMOpcode : size_t {
#define WABT_OPCODE(rtype, type1, type2, type3, memSize, prefix, code, name, \
                    text, decomp)                                            \
    name##Opcode,
#include "parser/opcode.def"
#undef WABT_OPCODE
    OpcodeKindEnd,
};

struct WASMCodeInfo {
    enum CodeType { ___,
                    I32,
                    I64,
                    F32,
                    F64,
                    V128 };
    WASMOpcode m_code;
    CodeType m_resultType;
    CodeType m_paramTypes[3];
    const char* m_name;

    size_t stackShrinkSize() const
    {
        ASSERT(m_code != WASMOpcode::OpcodeKindEnd);
        return codeTypeToMemorySize(m_paramTypes[0]) + codeTypeToMemorySize(m_paramTypes[1]) + codeTypeToMemorySize(m_paramTypes[2]);
    }

    size_t stackGrowSize() const
    {
        ASSERT(m_code != WASMOpcode::OpcodeKindEnd);
        return codeTypeToMemorySize(m_resultType);
    }

    static size_t codeTypeToMemorySize(CodeType tp)
    {
        switch (tp) {
        case I32:
            return Walrus::stackAllocatedSize<int32_t>();
        case F32:
            return Walrus::stackAllocatedSize<float>();
        case I64:
            return Walrus::stackAllocatedSize<int64_t>();
        case F64:
            return Walrus::stackAllocatedSize<double>();
        case V128:
            return 16;
        default:
            return 0;
        }
    }
};

WASMCodeInfo g_wasmCodeInfo[static_cast<size_t>(WASMOpcode::OpcodeKindEnd)] = {
#define WABT_OPCODE(rtype, type1, type2, type3, memSize, prefix, code, name, \
                    text, decomp)                                            \
    { WASMOpcode::name##Opcode,                                              \
      WASMCodeInfo::rtype,                                                   \
      { WASMCodeInfo::type1, WASMCodeInfo::type2, WASMCodeInfo::type3 },     \
      text },
#include "parser/opcode.def"
#undef WABT_OPCODE
};

static Walrus::Value::Type toValueKind(Type type)
{
    switch (type) {
    case Type::I32:
        return Walrus::Value::Type::I32;
    case Type::I64:
        return Walrus::Value::Type::I64;
    case Type::F32:
        return Walrus::Value::Type::F32;
    case Type::F64:
        return Walrus::Value::Type::F64;
    case Type::FuncRef:
        return Walrus::Value::Type::FuncRef;
    case Type::ExternRef:
        return Walrus::Value::Type::ExternRef;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static Walrus::SegmentMode toSegmentMode(uint8_t flags)
{
    enum SegmentFlags : uint8_t {
        SegFlagsNone = 0,
        SegPassive = 1, // bit 0: Is passive
        SegExplicitIndex = 2, // bit 1: Has explict index (Implies table 0 if absent)
        SegDeclared = 3, // Only used for declared segments
        SegUseElemExprs = 4, // bit 2: Is elemexpr (Or else index sequence)
        SegFlagMax = (SegUseElemExprs << 1) - 1, // All bits set.
    };

    if ((flags & SegDeclared) == SegDeclared) {
        return Walrus::SegmentMode::Declared;
    } else if ((flags & SegPassive) == SegPassive) {
        return Walrus::SegmentMode::Passive;
    } else {
        return Walrus::SegmentMode::Active;
    }
}

class WASMBinaryReader : public wabt::WASMBinaryReaderDelegate {
private:
    struct VMStackInfo {
        WASMBinaryReader& m_reader;
        size_t m_size;
        size_t m_position; // effective position (local values will have different position)
        size_t m_nonOptimizedPosition; // non-optimized position (same with m_functionStackSizeSoFar)
        size_t m_localIndex;

        VMStackInfo(WASMBinaryReader& reader, size_t size, size_t position, size_t nonOptimizedPosition, size_t localIndex)
            : m_reader(reader)
            , m_size(size)
            , m_position(position)
            , m_nonOptimizedPosition(nonOptimizedPosition)
            , m_localIndex(localIndex)
        {
            increaseRefCountIfNeeds();
        }

        VMStackInfo(const VMStackInfo& src)
            : m_reader(src.m_reader)
            , m_size(src.m_size)
            , m_position(src.m_position)
            , m_nonOptimizedPosition(src.m_nonOptimizedPosition)
            , m_localIndex(src.m_localIndex)
        {
            increaseRefCountIfNeeds();
        }

        ~VMStackInfo()
        {
            decreaseRefCountIfNeeds();
        }

        const VMStackInfo& operator=(const VMStackInfo& src)
        {
            decreaseRefCountIfNeeds();

            m_reader = src.m_reader;
            m_size = src.m_size;
            m_position = src.m_position;
            m_nonOptimizedPosition = src.m_nonOptimizedPosition;
            m_localIndex = src.m_localIndex;
            increaseRefCountIfNeeds();
            return *this;
        }

        bool hasValidLocalIndex() const
        {
            return m_localIndex != std::numeric_limits<size_t>::max();
        }

    private:
        void increaseRefCountIfNeeds()
        {
            if (m_localIndex != std::numeric_limits<size_t>::max()) {
                m_reader.m_localInfo[m_localIndex].m_refCount++;
            }
        }

        void decreaseRefCountIfNeeds()
        {
            if (m_localIndex != std::numeric_limits<size_t>::max()) {
                m_reader.m_localInfo[m_localIndex].m_refCount--;
            }
        }
    };

    struct BlockInfo {
        enum BlockType {
            IfElse,
            Loop,
            Block,
            TryCatch,
        };
        BlockType m_blockType;
        Type m_returnValueType;
        size_t m_position;
        std::vector<VMStackInfo> m_vmStack;
        std::vector<uint32_t> m_parameterPositions;
        uint32_t m_functionStackSizeSoFar;
        bool m_shouldRestoreVMStackAtEnd;
        bool m_byteCodeGenerationStopped;

        static_assert(sizeof(Walrus::JumpIfTrue) == sizeof(Walrus::JumpIfFalse), "");
        struct JumpToEndBrInfo {
            enum JumpToEndType {
                IsJump,
                IsJumpIf,
                IsBrTable,
            };

            JumpToEndType m_type;
            size_t m_position;
        };

        std::vector<JumpToEndBrInfo> m_jumpToEndBrInfo;

        BlockInfo(BlockType type, Type returnValueType, WASMBinaryReader& binaryReader)
            : m_blockType(type)
            , m_returnValueType(returnValueType)
            , m_position(0)
            , m_vmStack(binaryReader.m_vmStack)
            , m_functionStackSizeSoFar(binaryReader.m_functionStackSizeSoFar)
            , m_shouldRestoreVMStackAtEnd(false)
            , m_byteCodeGenerationStopped(false)
        {
            if (returnValueType.IsIndex() && binaryReader.m_result.m_functionTypes[returnValueType]->param().size()) {
                // record parameter positions
                auto& param = binaryReader.m_result.m_functionTypes[returnValueType]->param();
                auto endIter = binaryReader.m_vmStack.rbegin() + param.size();
                auto iter = binaryReader.m_vmStack.rbegin();
                while (iter != endIter) {
                    m_parameterPositions.push_back(iter->m_nonOptimizedPosition);
                    iter++;
                }

                // assign local values which use direct-access into general register
                endIter = binaryReader.m_vmStack.rbegin() + param.size();
                iter = binaryReader.m_vmStack.rbegin();
                while (iter != endIter) {
                    if (iter->m_position != iter->m_nonOptimizedPosition) {
                        binaryReader.generateMoveCodeIfNeeds(iter->m_position, iter->m_nonOptimizedPosition, iter->m_size);
                        iter->m_position = iter->m_nonOptimizedPosition;
                    }
                    iter++;
                }
            }

            m_position = binaryReader.m_currentFunction->currentByteCodeSize();
        }
    };

    size_t* m_readerOffsetPointer;
    size_t m_codeStartOffset;

    Walrus::ModuleFunction* m_currentFunction;
    Walrus::FunctionType* m_currentFunctionType;
    uint32_t m_initialFunctionStackSize;
    uint32_t m_functionStackSizeSoFar;
    uint32_t m_lastByteCodePosition;
    WASMOpcode m_lastPushedOpcode;
    uint32_t m_lastOpcode[2];

    std::vector<VMStackInfo> m_vmStack;
    std::vector<BlockInfo> m_blockInfo;
    struct CatchInfo {
        size_t m_tryCatchBlockDepth;
        size_t m_tryStart;
        size_t m_tryEnd;
        size_t m_catchStart;
        uint32_t m_tagIndex;
    };
    std::vector<CatchInfo> m_catchInfo;
    struct LocalInfo {
        size_t m_refCount;
        bool m_canUseDirectReference;
        LocalInfo()
            : m_refCount(0)
            , m_canUseDirectReference(true)
        {
        }

        void reset()
        {
            m_refCount = 0;
        }
    };
    std::vector<LocalInfo> m_localInfo;

    Walrus::Vector<uint8_t, std::allocator<uint8_t>> m_memoryInitData;

    uint32_t m_elementTableIndex;
    Walrus::Optional<Walrus::ModuleFunction*> m_elementModuleFunction;
    Walrus::Vector<uint32_t, std::allocator<uint32_t>> m_elementFunctionIndex;
    Walrus::SegmentMode m_segmentMode;

    Walrus::WASMParsingResult m_result;

    virtual void OnSetOffsetAddress(size_t* ptr) override
    {
        m_readerOffsetPointer = ptr;
    }

    size_t pushVMStack(size_t size)
    {
        auto pos = m_functionStackSizeSoFar;
        pushVMStack(size, pos);
        return pos;
    }

    void resetFunctionCodeDataFromHere()
    {
        m_skipValidationUntil = *m_readerOffsetPointer;
        *m_readerOffsetPointer = m_codeStartOffset;

        m_currentFunction->m_byteCode.clear();
        m_currentFunction->m_catchInfo.clear();
        m_blockInfo.clear();
        m_catchInfo.clear();

        m_functionStackSizeSoFar = m_initialFunctionStackSize;
        m_lastByteCodePosition = 0;
        m_lastPushedOpcode = WASMOpcode::OpcodeKindEnd;
        m_lastOpcode[0] = m_lastOpcode[1] = 0;

        m_vmStack.clear();

        for (auto& info : m_localInfo) {
            info.reset();
        }
    }

    void pushVMStack(size_t size, size_t pos, size_t localIndex = std::numeric_limits<size_t>::max())
    {
        m_vmStack.push_back(VMStackInfo(*this, size, pos, m_functionStackSizeSoFar, localIndex));
        m_functionStackSizeSoFar += size;
        if (UNLIKELY(m_functionStackSizeSoFar > std::numeric_limits<Walrus::ByteCodeStackOffset>::max())) {
            throw std::string("too many stack usage. we could not support this(yet).");
        }
        m_currentFunction->m_requiredStackSize = std::max(
            m_currentFunction->m_requiredStackSize, m_functionStackSizeSoFar);
    }

    VMStackInfo popVMStackInfo()
    {
        auto info = m_vmStack.back();
        m_functionStackSizeSoFar -= info.m_size;
        m_vmStack.pop_back();
        return info;
    }

    size_t popVMStackSize()
    {
        return popVMStackInfo().m_size;
    }

    size_t popVMStack()
    {
        return popVMStackInfo().m_position;
    }

    size_t peekVMStackSize()
    {
        return m_vmStack.back().m_size;
    }

    size_t peekVMStack()
    {
        return m_vmStack.back().m_position;
    }

    const VMStackInfo& peekVMStackInfo()
    {
        return m_vmStack.back();
    }

    void beginFunction(Walrus::ModuleFunction* mf)
    {
        m_currentFunction = mf;
        m_currentFunctionType = mf->functionType();
        m_localInfo.clear();
        m_localInfo.reserve(m_currentFunctionType->param().size());
        for (size_t i = 0; i < m_currentFunctionType->param().size(); i++) {
            m_localInfo.push_back(LocalInfo());
        }
        m_initialFunctionStackSize = m_functionStackSizeSoFar = m_currentFunctionType->paramStackSize();
        m_lastByteCodePosition = 0;
        m_lastPushedOpcode = WASMOpcode::OpcodeKindEnd;
        m_currentFunction->m_requiredStackSize = std::max(
            m_currentFunction->m_requiredStackSize, m_functionStackSizeSoFar);
    }

    void endFunction()
    {
        m_currentFunction = nullptr;
        m_currentFunctionType = nullptr;
        m_vmStack.clear();
        m_shouldContinueToGenerateByteCode = true;
    }

    template <typename CodeType>
    void pushByteCode(const CodeType& code, WASMOpcode opcode)
    {
        m_lastByteCodePosition = m_currentFunction->currentByteCodeSize();
        m_lastPushedOpcode = opcode;
        m_currentFunction->pushByteCode(code);
    }

    bool canUseDirectReference(uint32_t localIndex, uint32_t pos)
    {
        for (const auto& bi : m_blockInfo) {
            for (uint32_t p : bi.m_parameterPositions) {
                if (pos == p) {
                    return false;
                }
            }
        }
        return m_localInfo[localIndex].m_canUseDirectReference;
    }

public:
    WASMBinaryReader()
        : m_readerOffsetPointer(nullptr)
        , m_codeStartOffset(0)
        , m_currentFunction(nullptr)
        , m_currentFunctionType(nullptr)
        , m_initialFunctionStackSize(0)
        , m_functionStackSizeSoFar(0)
        , m_lastByteCodePosition(0)
        , m_lastPushedOpcode(WASMOpcode::OpcodeKindEnd)
        , m_lastOpcode{ 0, 0 }
        , m_elementTableIndex(0)
        , m_segmentMode(Walrus::SegmentMode::None)
    {
    }

    ~WASMBinaryReader()
    {
        // clear stack first! because vmStack refer localInfo
        m_vmStack.clear();
        m_localInfo.clear();

        m_result.clear();
    }

    // should be allocated on the stack
    static void* operator new(size_t) = delete;
    static void* operator new[](size_t) = delete;

    virtual void BeginModule(uint32_t version) override
    {
        m_result.m_version = version;
    }

    virtual void EndModule() override {}

    virtual void OnTypeCount(Index count) override
    {
        // TODO reserve vector if possible
    }

    virtual void OnFuncType(Index index,
                            Index paramCount,
                            Type* paramTypes,
                            Index resultCount,
                            Type* resultTypes) override
    {
        Walrus::ValueTypeVector* param = new Walrus::ValueTypeVector();
        param->reserve(paramCount);
        for (size_t i = 0; i < paramCount; i++) {
            param->push_back(toValueKind(paramTypes[i]));
        }
        Walrus::ValueTypeVector* result = new Walrus::ValueTypeVector();
        for (size_t i = 0; i < resultCount; i++) {
            result->push_back(toValueKind(resultTypes[i]));
        }
        ASSERT(index == m_result.m_functionTypes.size());
        m_result.m_functionTypes.push_back(new Walrus::FunctionType(param, result));
    }

    virtual void OnImportCount(Index count) override
    {
        m_result.m_imports.reserve(count);
    }

    virtual void OnImportFunc(Index importIndex,
                              std::string moduleName,
                              std::string fieldName,
                              Index funcIndex,
                              Index sigIndex) override
    {
        ASSERT(m_result.m_functions.size() == funcIndex);
        ASSERT(m_result.m_imports.size() == importIndex);
        Walrus::FunctionType* ft = m_result.m_functionTypes[sigIndex];
        m_result.m_functions.push_back(
            new Walrus::ModuleFunction(ft));
        m_result.m_imports.push_back(new Walrus::ImportType(
            Walrus::ImportType::Function,
            moduleName, fieldName, ft));
    }

    virtual void OnImportGlobal(Index importIndex, std::string moduleName, std::string fieldName, Index globalIndex, Type type, bool mutable_) override
    {
        ASSERT(globalIndex == m_result.m_globalTypes.size());
        ASSERT(m_result.m_imports.size() == importIndex);
        m_result.m_globalTypes.push_back(new Walrus::GlobalType(toValueKind(type), mutable_));
        m_result.m_imports.push_back(new Walrus::ImportType(
            Walrus::ImportType::Global,
            moduleName, fieldName, m_result.m_globalTypes[globalIndex]));
    }

    virtual void OnImportTable(Index importIndex, std::string moduleName, std::string fieldName, Index tableIndex, Type type, size_t initialSize, size_t maximumSize) override
    {
        ASSERT(tableIndex == m_result.m_tableTypes.size());
        ASSERT(m_result.m_imports.size() == importIndex);
        ASSERT(type == Type::FuncRef || type == Type::ExternRef);

        m_result.m_tableTypes.push_back(new Walrus::TableType(type == Type::FuncRef ? Walrus::Value::Type::FuncRef : Walrus::Value::Type::ExternRef, initialSize, maximumSize));
        m_result.m_imports.push_back(new Walrus::ImportType(
            Walrus::ImportType::Table,
            moduleName, fieldName, m_result.m_tableTypes[tableIndex]));
    }

    virtual void OnImportMemory(Index importIndex, std::string moduleName, std::string fieldName, Index memoryIndex, size_t initialSize, size_t maximumSize) override
    {
        ASSERT(memoryIndex == m_result.m_memoryTypes.size());
        ASSERT(m_result.m_imports.size() == importIndex);
        m_result.m_memoryTypes.push_back(new Walrus::MemoryType(initialSize, maximumSize));
        m_result.m_imports.push_back(new Walrus::ImportType(
            Walrus::ImportType::Memory,
            moduleName, fieldName, m_result.m_memoryTypes[memoryIndex]));
    }

    virtual void OnImportTag(Index importIndex, std::string moduleName, std::string fieldName, Index tagIndex, Index sigIndex) override
    {
        ASSERT(tagIndex == m_result.m_tagTypes.size());
        ASSERT(m_result.m_imports.size() == importIndex);
        m_result.m_tagTypes.push_back(new Walrus::TagType(sigIndex));
        m_result.m_imports.push_back(new Walrus::ImportType(
            Walrus::ImportType::Tag,
            moduleName, fieldName, m_result.m_tagTypes[tagIndex]));
    }

    virtual void OnExportCount(Index count) override
    {
        m_result.m_exports.reserve(count);
    }

    virtual void OnExport(int kind, Index exportIndex, std::string name, Index itemIndex) override
    {
        ASSERT(m_result.m_exports.size() == exportIndex);
        m_result.m_exports.push_back(new Walrus::ExportType(static_cast<Walrus::ExportType::Type>(kind), name, itemIndex));
    }

    /* Table section */
    virtual void OnTableCount(Index count) override
    {
        m_result.m_tableTypes.reserve(count);
    }

    virtual void OnTable(Index index, Type type, size_t initialSize, size_t maximumSize) override
    {
        ASSERT(index == m_result.m_tableTypes.size());
        ASSERT(type == Type::FuncRef || type == Type::ExternRef);
        m_result.m_tableTypes.push_back(new Walrus::TableType(type == Type::FuncRef ? Walrus::Value::Type::FuncRef : Walrus::Value::Type::ExternRef, initialSize, maximumSize));
    }

    virtual void OnElemSegmentCount(Index count) override
    {
        m_result.m_elements.reserve(count);
    }

    virtual void BeginElemSegment(Index index, Index tableIndex, uint8_t flags) override
    {
        m_elementTableIndex = tableIndex;
        m_elementModuleFunction = nullptr;
        m_segmentMode = toSegmentMode(flags);
    }

    virtual void BeginElemSegmentInitExpr(Index index) override
    {
        beginFunction(new Walrus::ModuleFunction(Walrus::Store::getDefaultFunctionType(Walrus::Value::I32)));
    }

    virtual void EndElemSegmentInitExpr(Index index) override
    {
        m_elementModuleFunction = m_currentFunction;
        endFunction();
    }

    virtual void OnElemSegmentElemType(Index index, Type elemType) override
    {
    }

    virtual void OnElemSegmentElemExprCount(Index index, Index count) override
    {
        m_elementFunctionIndex.reserve(count);
    }

    virtual void OnElemSegmentElemExpr_RefNull(Index segmentIndex, Type type) override
    {
        m_elementFunctionIndex.push_back(std::numeric_limits<uint32_t>::max());
    }

    virtual void OnElemSegmentElemExpr_RefFunc(Index segmentIndex, Index funcIndex) override
    {
        m_elementFunctionIndex.push_back(funcIndex);
    }

    virtual void EndElemSegment(Index index) override
    {
        ASSERT(m_result.m_elements.size() == index);
        if (m_elementModuleFunction) {
            m_result.m_elements.push_back(new Walrus::Element(m_segmentMode, m_elementTableIndex, m_elementModuleFunction.value(), std::move(m_elementFunctionIndex)));
        } else {
            m_result.m_elements.push_back(new Walrus::Element(m_segmentMode, m_elementTableIndex, std::move(m_elementFunctionIndex)));
        }

        m_elementModuleFunction = nullptr;
        m_elementTableIndex = 0;
        m_elementFunctionIndex.clear();
        m_segmentMode = Walrus::SegmentMode::None;
    }

    /* Memory section */
    virtual void OnMemoryCount(Index count) override
    {
        m_result.m_memoryTypes.reserve(count);
    }

    virtual void OnMemory(Index index, size_t initialSize, size_t maximumSize) override
    {
        ASSERT(index == m_result.m_memoryTypes.size());
        m_result.m_memoryTypes.push_back(new Walrus::MemoryType(initialSize, maximumSize));
    }

    virtual void OnDataSegmentCount(Index count) override
    {
        m_result.m_datas.reserve(count);
    }

    virtual void BeginDataSegment(Index index, Index memoryIndex, uint8_t flags) override
    {
        ASSERT(index == m_result.m_datas.size());
        beginFunction(new Walrus::ModuleFunction(Walrus::Store::getDefaultFunctionType(Walrus::Value::I32)));
    }

    virtual void BeginDataSegmentInitExpr(Index index) override
    {
    }

    virtual void EndDataSegmentInitExpr(Index index) override
    {
    }

    virtual void OnDataSegmentData(Index index, const void* data, Address size) override
    {
        m_memoryInitData.resizeWithUninitializedValues(size);
        memcpy(m_memoryInitData.data(), data, size);
    }

    virtual void EndDataSegment(Index index) override
    {
        ASSERT(index == m_result.m_datas.size());
        m_result.m_datas.push_back(new Walrus::Data(m_currentFunction, std::move(m_memoryInitData)));
        endFunction();
    }

    /* Function section */
    virtual void OnFunctionCount(Index count) override
    {
        m_result.m_functions.reserve(count);
    }

    virtual void OnFunction(Index index, Index sigIndex) override
    {
        ASSERT(m_currentFunction == nullptr);
        ASSERT(m_currentFunctionType == nullptr);
        ASSERT(m_result.m_functions.size() == index);
        m_result.m_functions.push_back(new Walrus::ModuleFunction(m_result.m_functionTypes[sigIndex]));
    }

    virtual void OnGlobalCount(Index count) override
    {
        m_result.m_globalTypes.reserve(count);
    }

    virtual void BeginGlobal(Index index, Type type, bool mutable_) override
    {
        ASSERT(m_result.m_globalTypes.size() == index);
        m_result.m_globalTypes.push_back(new Walrus::GlobalType(toValueKind(type), mutable_));
    }

    virtual void BeginGlobalInitExpr(Index index) override
    {
        auto ft = Walrus::Store::getDefaultFunctionType(m_result.m_globalTypes[index]->type());
        Walrus::ModuleFunction* mf = new Walrus::ModuleFunction(ft);
        m_result.m_globalTypes[index]->setFunction(mf);
        beginFunction(mf);
    }

    virtual void EndGlobalInitExpr(Index index) override
    {
        endFunction();
    }

    virtual void EndGlobal(Index index) override
    {
    }

    virtual void EndGlobalSection() override
    {
    }

    virtual void OnTagCount(Index count) override
    {
        m_result.m_tagTypes.reserve(count);
    }

    virtual void OnTagType(Index index, Index sigIndex) override
    {
        ASSERT(index == m_result.m_tagTypes.size());
        m_result.m_tagTypes.push_back(new Walrus::TagType(sigIndex));
    }

    virtual void OnStartFunction(Index funcIndex) override
    {
        m_result.m_seenStartAttribute = true;
        m_result.m_start = funcIndex;
    }

    virtual void BeginFunctionBody(Index index, Offset size) override
    {
        ASSERT(m_currentFunction == nullptr);
        beginFunction(m_result.m_functions[index]);
    }

    virtual void OnLocalDeclCount(Index count) override
    {
        m_currentFunction->m_local.reserve(count);
        m_localInfo.reserve(count + m_currentFunctionType->param().size());
    }

    virtual void OnLocalDecl(Index decl_index, Index count, Type type) override
    {
        while (count) {
            auto wType = toValueKind(type);
            m_currentFunction->m_local.push_back(wType);
            m_localInfo.push_back(LocalInfo());
            auto sz = Walrus::valueSizeInStack(wType);
            m_initialFunctionStackSize += sz;
            m_functionStackSizeSoFar += sz;
            m_currentFunction->m_requiredStackSizeDueToLocal += sz;
            count--;
        }
        m_currentFunction->m_requiredStackSize = std::max(
            m_currentFunction->m_requiredStackSize, m_functionStackSizeSoFar);
    }

    virtual void OnStartReadInstructions() override
    {
        m_codeStartOffset = *m_readerOffsetPointer;
    }

    virtual void OnOpcode(uint32_t opcode) override
    {
        m_lastOpcode[1] = m_lastOpcode[0];
        m_lastOpcode[0] = opcode;
    }

    virtual void OnCallExpr(uint32_t index) override
    {
        auto functionType = m_result.m_functions[index]->functionType();
        auto callPos = m_currentFunction->currentByteCodeSize();
        pushByteCode(Walrus::Call(index, functionType->param().size() + functionType->result().size()
#if !defined(NDEBUG)
                                             ,
                                  functionType
#endif
                                  ),
                     WASMOpcode::CallOpcode);

        m_currentFunction->expandByteCode(sizeof(Walrus::ByteCodeStackOffset) * (functionType->param().size() + functionType->result().size()));
        auto code = m_currentFunction->peekByteCode<Walrus::Call>(callPos);

        size_t c = 0;
        size_t siz = functionType->param().size();
        for (size_t i = 0; i < siz; i++) {
            ASSERT(peekVMStackSize() == Walrus::valueSizeInStack(functionType->param()[siz - i - 1]));
            code->stackOffsets()[siz - c++ - 1] = popVMStack();
        }
        siz = functionType->result().size();
        for (size_t i = 0; i < siz; i++) {
            code->stackOffsets()[c++] = pushVMStack(Walrus::valueSizeInStack(functionType->result()[i]));
        }
    }

    virtual void OnCallIndirectExpr(Index sigIndex, Index tableIndex) override
    {
        ASSERT(peekVMStackSize() == Walrus::valueSizeInStack(toValueKind(Type::I32)));
        auto functionType = m_result.m_functionTypes[sigIndex];
        auto callPos = m_currentFunction->currentByteCodeSize();
        pushByteCode(Walrus::CallIndirect(popVMStack(), tableIndex, functionType), WASMOpcode::CallIndirectOpcode);
        m_currentFunction->expandByteCode(sizeof(Walrus::ByteCodeStackOffset) * (functionType->param().size() + functionType->result().size()));

        auto code = m_currentFunction->peekByteCode<Walrus::CallIndirect>(callPos);

        size_t c = 0;
        size_t siz = functionType->param().size();
        for (size_t i = 0; i < siz; i++) {
            ASSERT(peekVMStackSize() == Walrus::valueSizeInStack(functionType->param()[siz - i - 1]));
            code->stackOffsets()[siz - c++ - 1] = popVMStack();
        }
        siz = functionType->result().size();
        for (size_t i = 0; i < siz; i++) {
            code->stackOffsets()[c++] = pushVMStack(Walrus::valueSizeInStack(functionType->result()[i]));
        }
    }

    virtual void OnI32ConstExpr(uint32_t value) override
    {
        pushByteCode(Walrus::Const32(pushVMStack(Walrus::valueSizeInStack(Walrus::Value::Type::I32)), value), WASMOpcode::I32ConstOpcode);
    }

    virtual void OnI64ConstExpr(uint64_t value) override
    {
        pushByteCode(Walrus::Const64(pushVMStack(Walrus::valueSizeInStack(Walrus::Value::Type::I64)), value), WASMOpcode::I64ConstOpcode);
    }

    virtual void OnF32ConstExpr(uint32_t value) override
    {
        pushByteCode(Walrus::Const32(pushVMStack(Walrus::valueSizeInStack(Walrus::Value::Type::F32)), value), WASMOpcode::F32ConstOpcode);
    }

    virtual void OnF64ConstExpr(uint64_t value) override
    {
        pushByteCode(Walrus::Const64(pushVMStack(Walrus::valueSizeInStack(Walrus::Value::Type::F64)), value), WASMOpcode::F64ConstOpcode);
    }

    std::pair<uint32_t, uint32_t> resolveLocalOffsetAndSize(Index localIndex)
    {
        if (localIndex < m_currentFunctionType->param().size()) {
            size_t offset = 0;
            for (Index i = 0; i < localIndex; i++) {
                offset += Walrus::valueSizeInStack(m_currentFunctionType->param()[i]);
            }
            return std::make_pair(offset, Walrus::valueSizeInStack(m_currentFunctionType->param()[localIndex]));
        } else {
            localIndex -= m_currentFunctionType->param().size();
            size_t offset = m_currentFunctionType->paramStackSize();
            for (Index i = 0; i < localIndex; i++) {
                offset += Walrus::valueSizeInStack(m_currentFunction->m_local[i]);
            }
            return std::make_pair(offset, Walrus::valueSizeInStack(m_currentFunction->m_local[localIndex]));
        }
    }

    Index resolveLocalIndexFromStackPosition(size_t pos)
    {
        ASSERT(pos < m_initialFunctionStackSize);
        if (pos <= m_currentFunctionType->paramStackSize()) {
            Index idx = 0;
            size_t offset = 0;
            while (true) {
                if (offset == pos) {
                    return idx;
                }
                offset += Walrus::valueSizeInStack(m_currentFunctionType->param()[idx]);
                idx++;
            }
        }
        pos -= m_currentFunctionType->paramStackSize();
        Index idx = 0;
        size_t offset = 0;
        while (true) {
            if (offset == pos) {
                return idx + m_currentFunctionType->param().size();
            }
            offset += Walrus::valueSizeInStack(m_currentFunction->m_local[idx]);
            idx++;
        }
    }

    virtual void OnLocalGetExpr(Index localIndex) override
    {
        auto r = resolveLocalOffsetAndSize(localIndex);
        if (canUseDirectReference(localIndex, m_functionStackSizeSoFar)) {
            pushVMStack(r.second, r.first, localIndex);
        } else {
            auto pos = m_functionStackSizeSoFar;
            pushVMStack(r.second, pos, localIndex);
            generateMoveCodeIfNeeds(r.first, pos, r.second);
        }
    }

    bool omitUpdateLocalValueIfPossible(Index localIndex, std::pair<uint32_t, uint32_t> localOffsetAndSize, const VMStackInfo& stack)
    {
        if (canUseDirectReference(localIndex, stack.m_position) && stack.m_position != localOffsetAndSize.first && !stack.hasValidLocalIndex()) {
            // we should check last opcode and bytecode are same
            // because some opcode omitted by optimization
            // eg) (i32.add) (local.get 0) ;; local.get 0 can be omitted by direct access
            if (m_lastOpcode[1] == static_cast<uint32_t>(m_lastPushedOpcode) && isBinaryOperation(m_lastPushedOpcode)) {
                m_currentFunction->peekByteCode<Walrus::BinaryOperation>(m_lastByteCodePosition)->setDstOffset(localOffsetAndSize.first);
            } else if (m_lastPushedOpcode == WASMOpcode::Const32Opcode) {
                m_currentFunction->peekByteCode<Walrus::Const32>(m_lastByteCodePosition)->setDstOffset(localOffsetAndSize.first);
            } else if (m_lastPushedOpcode == WASMOpcode::Const64Opcode) {
                m_currentFunction->peekByteCode<Walrus::Const64>(m_lastByteCodePosition)->setDstOffset(localOffsetAndSize.first);
            } else {
                return false;
            }
            return true;
        }
        return false;
    }

    virtual void OnLocalSetExpr(Index localIndex) override
    {
        auto r = resolveLocalOffsetAndSize(localIndex);
        if (m_localInfo[localIndex].m_refCount && m_localInfo[localIndex].m_canUseDirectReference) {
            // src and dst are same!
            // example) (local.get 0) (local.set 0) ;; w/direct access
            if (peekVMStackInfo().m_position != r.first) {
                // rewind generating bytecode
                m_localInfo[localIndex].m_canUseDirectReference = false;
                resetFunctionCodeDataFromHere();
                return;
            }
        }

        ASSERT(r.second == peekVMStackSize());
        auto src = popVMStackInfo();
        if (!omitUpdateLocalValueIfPossible(localIndex, r, src)) {
            generateMoveCodeIfNeeds(src.m_position, r.first, r.second);
        }
    }

    virtual void OnLocalTeeExpr(Index localIndex) override
    {
        if (m_localInfo[localIndex].m_refCount && m_localInfo[localIndex].m_canUseDirectReference) {
            m_localInfo[localIndex].m_canUseDirectReference = false;
            resetFunctionCodeDataFromHere();
            return;
        }


        auto r = resolveLocalOffsetAndSize(localIndex);
        ASSERT(r.second == peekVMStackSize());
        auto dstInfo = peekVMStackInfo();

        if (omitUpdateLocalValueIfPossible(localIndex, r, dstInfo)) {
            auto oldInfo = popVMStackInfo();
            pushVMStack(oldInfo.m_size, r.first, localIndex);
        } else {
            generateMoveCodeIfNeeds(dstInfo.m_position, r.first, r.second);
        }
    }

    virtual void OnGlobalGetExpr(Index index) override
    {
        auto sz = Walrus::valueSizeInStack(m_result.m_globalTypes[index]->type());
        auto stackPos = pushVMStack(sz);
        if (sz == 4) {
            pushByteCode(Walrus::GlobalGet32(stackPos, index), WASMOpcode::GlobalGetOpcode);
        } else {
            ASSERT(sz == 8);
            pushByteCode(Walrus::GlobalGet64(stackPos, index), WASMOpcode::GlobalGetOpcode);
        }
    }

    virtual void OnGlobalSetExpr(Index index) override
    {
        auto stackPos = peekVMStack();

        auto sz = Walrus::valueSizeInStack(m_result.m_globalTypes[index]->type());
        if (sz == 4) {
            ASSERT(peekVMStackSize() == 4);
            pushByteCode(Walrus::GlobalSet32(stackPos, index), WASMOpcode::GlobalSetOpcode);
        } else {
            ASSERT(sz == 8);
            ASSERT(peekVMStackSize() == 8);
            pushByteCode(Walrus::GlobalSet64(stackPos, index), WASMOpcode::GlobalSetOpcode);
        }
        popVMStack();
    }

    virtual void OnDropExpr() override
    {
        popVMStack();
    }

    virtual void OnBinaryExpr(uint32_t opcode) override
    {
        auto code = static_cast<WASMOpcode>(opcode);
        ASSERT(WASMCodeInfo::codeTypeToMemorySize(g_wasmCodeInfo[opcode].m_paramTypes[0]) == peekVMStackSize());
        auto src1 = popVMStack();
        ASSERT(WASMCodeInfo::codeTypeToMemorySize(g_wasmCodeInfo[opcode].m_paramTypes[1]) == peekVMStackSize());
        auto src0 = popVMStack();
        auto dst = pushVMStack(WASMCodeInfo::codeTypeToMemorySize(g_wasmCodeInfo[opcode].m_resultType));
        generateBinaryCode(code, src0, src1, dst);
    }

    virtual void OnUnaryExpr(uint32_t opcode) override
    {
        auto code = static_cast<WASMOpcode>(opcode);
        ASSERT(WASMCodeInfo::codeTypeToMemorySize(g_wasmCodeInfo[opcode].m_paramTypes[0]) == peekVMStackSize());
        auto src = popVMStack();
        auto dst = pushVMStack(WASMCodeInfo::codeTypeToMemorySize(g_wasmCodeInfo[opcode].m_resultType));
        switch (code) {
        case WASMOpcode::I32ReinterpretF32Opcode:
        case WASMOpcode::I64ReinterpretF64Opcode:
        case WASMOpcode::F32ReinterpretI32Opcode:
        case WASMOpcode::F64ReinterpretI64Opcode:
            generateMoveCodeIfNeeds(src, dst, WASMCodeInfo::codeTypeToMemorySize(g_wasmCodeInfo[opcode].m_resultType));
            break;
        default:
            generateUnaryCode(code, src, dst);
            break;
        }
    }

    virtual void OnIfExpr(Type sigType) override
    {
        ASSERT(peekVMStackSize() == Walrus::valueSizeInStack(toValueKind(Type::I32)));
        auto stackPos = popVMStack();

        BlockInfo b(BlockInfo::IfElse, sigType, *this);
        b.m_jumpToEndBrInfo.push_back({ BlockInfo::JumpToEndBrInfo::IsJumpIf, b.m_position });
        m_blockInfo.push_back(b);
        pushByteCode(Walrus::JumpIfFalse(stackPos), WASMOpcode::IfOpcode);
    }

    void restoreVMStackBy(const BlockInfo& blockInfo)
    {
        m_vmStack = blockInfo.m_vmStack;
        m_functionStackSizeSoFar = blockInfo.m_functionStackSizeSoFar;
    }

    void restoreVMStackRegardToPartOfBlockEnd(const BlockInfo& blockInfo)
    {
        if (blockInfo.m_shouldRestoreVMStackAtEnd) {
            restoreVMStackBy(blockInfo);
        } else if (blockInfo.m_returnValueType.IsIndex()) {
            auto ft = m_result.m_functionTypes[blockInfo.m_returnValueType];
            if (ft->param().size()) {
                restoreVMStackBy(blockInfo);
            } else {
                const auto& result = ft->result();
                for (size_t i = 0; i < result.size(); i++) {
                    ASSERT(peekVMStackSize() == Walrus::valueSizeInStack(result[result.size() - i - 1]));
                    popVMStackSize();
                }
            }
        } else if (blockInfo.m_returnValueType != Type::Void) {
            ASSERT(peekVMStackSize() == Walrus::valueSizeInStack(toValueKind(blockInfo.m_returnValueType)));
            popVMStackSize();
        }
    }

    void keepSubResultsIfNeeds()
    {
        BlockInfo& blockInfo = m_blockInfo.back();
        if ((blockInfo.m_returnValueType.IsIndex() && m_result.m_functionTypes[blockInfo.m_returnValueType]->result().size())
            || blockInfo.m_returnValueType != Type::Void) {
            blockInfo.m_shouldRestoreVMStackAtEnd = true;
            auto dropSize = dropStackValuesBeforeBrIfNeeds(0);
            if (dropSize.second) {
                generateMoveValuesCodeRegardToDrop(dropSize);
            }
        }
    }

    virtual void OnElseExpr() override
    {
        keepSubResultsIfNeeds();
        BlockInfo& blockInfo = m_blockInfo.back();
        blockInfo.m_jumpToEndBrInfo.erase(blockInfo.m_jumpToEndBrInfo.begin());
        blockInfo.m_jumpToEndBrInfo.push_back({ BlockInfo::JumpToEndBrInfo::IsJump, m_currentFunction->currentByteCodeSize() });
        pushByteCode(Walrus::Jump(), WASMOpcode::ElseOpcode);
        ASSERT(blockInfo.m_blockType == BlockInfo::IfElse);
        restoreVMStackRegardToPartOfBlockEnd(blockInfo);
        m_currentFunction->peekByteCode<Walrus::JumpIfFalse>(blockInfo.m_position)
            ->setOffset(m_currentFunction->currentByteCodeSize() - blockInfo.m_position);
    }

    virtual void OnLoopExpr(Type sigType) override
    {
        BlockInfo b(BlockInfo::Loop, sigType, *this);
        m_blockInfo.push_back(b);
    }

    virtual void OnBlockExpr(Type sigType) override
    {
        BlockInfo b(BlockInfo::Block, sigType, *this);
        m_blockInfo.push_back(b);
    }

    BlockInfo& findBlockInfoInBr(Index depth)
    {
        ASSERT(m_blockInfo.size());
        auto iter = m_blockInfo.rbegin();
        while (depth) {
            iter++;
            depth--;
        }
        return *iter;
    }

    void stopToGenerateByteCodeWhileBlockEnd()
    {
        if (m_resumeGenerateByteCodeAfterNBlockEnd) {
            return;
        }

        if (m_blockInfo.size()) {
            m_resumeGenerateByteCodeAfterNBlockEnd = 1;
            m_blockInfo.back().m_shouldRestoreVMStackAtEnd = true;
            m_blockInfo.back().m_byteCodeGenerationStopped = true;
        }
        m_shouldContinueToGenerateByteCode = false;
    }

    // return drop size, parameter size
    std::pair<size_t, size_t> dropStackValuesBeforeBrIfNeeds(Index depth)
    {
        size_t dropValueSize = 0;
        size_t parameterSize = 0;
        if (depth < m_blockInfo.size()) {
            auto iter = m_blockInfo.rbegin() + depth;
            if (iter->m_vmStack.size() < m_vmStack.size()) {
                size_t start = iter->m_vmStack.size();
                for (size_t i = start; i < m_vmStack.size(); i++) {
                    dropValueSize += m_vmStack[i].m_size;
                }

                if (iter->m_blockType == BlockInfo::Loop) {
                    if (iter->m_returnValueType.IsIndex()) {
                        auto ft = m_result.m_functionTypes[iter->m_returnValueType];
                        dropValueSize += ft->paramStackSize();
                        parameterSize += ft->paramStackSize();
                    }
                } else {
                    if (iter->m_returnValueType.IsIndex()) {
                        auto ft = m_result.m_functionTypes[iter->m_returnValueType];
                        const auto& result = ft->result();
                        for (size_t i = 0; i < result.size(); i++) {
                            parameterSize += Walrus::valueSizeInStack(result[i]);
                        }
                    } else if (iter->m_returnValueType != Type::Void) {
                        parameterSize += Walrus::valueSizeInStack(toValueKind(iter->m_returnValueType));
                    }
                }
            }
        } else if (m_blockInfo.size()) {
            auto iter = m_blockInfo.begin();
            size_t start = iter->m_vmStack.size();
            for (size_t i = start; i < m_vmStack.size(); i++) {
                dropValueSize += m_vmStack[i].m_size;
            }
        }

        return std::make_pair(dropValueSize, parameterSize);
    }

    void generateMoveCodeIfNeeds(size_t srcPosition, size_t dstPosition, size_t size)
    {
        if (srcPosition != dstPosition) {
            if (size == 4) {
                pushByteCode(Walrus::Move32(srcPosition, dstPosition), WASMOpcode::Move32Opcode);
            } else {
                ASSERT(size == 8);
                pushByteCode(Walrus::Move64(srcPosition, dstPosition), WASMOpcode::Move64Opcode);
            }
        }
    }

    void generateMoveValuesCodeRegardToDrop(std::pair<size_t, size_t> dropSize)
    {
        ASSERT(dropSize.second);
        int64_t remainSize = dropSize.second;
        auto srcIter = m_vmStack.rbegin();
        while (true) {
            remainSize -= srcIter->m_size;
            if (!remainSize) {
                break;
            }
            if (remainSize < 0) {
                // stack mismatch! we don't need to generate code
                return;
            }
            srcIter++;
        }

        remainSize = dropSize.first;
        auto dstIter = m_vmStack.rbegin();
        while (true) {
            remainSize -= dstIter->m_size;
            if (!remainSize) {
                break;
            }
            if (remainSize < 0) {
                // stack mismatch! we don't need to generate code
                return;
            }
            dstIter++;
        }

        // reverse order copy to protect newer values
        remainSize = dropSize.second;
        while (true) {
            generateMoveCodeIfNeeds(srcIter->m_position, dstIter->m_nonOptimizedPosition, srcIter->m_size);
            remainSize -= srcIter->m_size;
            if (!remainSize) {
                break;
            }
            srcIter--;
            dstIter--;
        }
    }

    void generateEndCode()
    {
        if (UNLIKELY(m_currentFunctionType->result().size() > m_vmStack.size())) {
            // error case of global init expr
            return;
        }
        auto pos = m_currentFunction->currentByteCodeSize();
        pushByteCode(Walrus::End(m_currentFunctionType->result().size()), WASMOpcode::EndOpcode);

        auto& result = m_currentFunctionType->result();
        m_currentFunction->expandByteCode(sizeof(Walrus::ByteCodeStackOffset) * result.size());
        Walrus::End* end = m_currentFunction->peekByteCode<Walrus::End>(pos);
        for (size_t i = 0; i < result.size(); i++) {
            end->resultOffsets()[result.size() - i - 1] = (m_vmStack.rbegin() + i)->m_position;
        }
    }

    void generateFunctionReturnCode(bool shouldClearVMStack = false)
    {
        for (size_t i = 0; i < m_currentFunctionType->result().size(); i++) {
            ASSERT((m_vmStack.rbegin() + i)->m_size == Walrus::valueSizeInStack(m_currentFunctionType->result()[m_currentFunctionType->result().size() - i - 1]));
        }
        generateEndCode();
        if (shouldClearVMStack) {
            auto dropSize = dropStackValuesBeforeBrIfNeeds(m_blockInfo.size()).first;
            while (dropSize) {
                dropSize -= popVMStackSize();
            }
        } else {
            for (size_t i = 0; i < m_currentFunctionType->result().size(); i++) {
                popVMStackSize();
            }
            stopToGenerateByteCodeWhileBlockEnd();
        }

        if (!m_blockInfo.size()) {
            // stop to generate bytecode from here!
            m_shouldContinueToGenerateByteCode = false;
            m_resumeGenerateByteCodeAfterNBlockEnd = 0;
        }
    }

    virtual void OnBrExpr(Index depth) override
    {
        if (m_blockInfo.size() == depth) {
            // this case acts like return
            generateFunctionReturnCode(true);
            return;
        }
        auto& blockInfo = findBlockInfoInBr(depth);
        auto offset = (int32_t)blockInfo.m_position - (int32_t)m_currentFunction->currentByteCodeSize();
        auto dropSize = dropStackValuesBeforeBrIfNeeds(depth);
        if (dropSize.second) {
            generateMoveValuesCodeRegardToDrop(dropSize);
        }
        if (blockInfo.m_blockType == BlockInfo::Block || blockInfo.m_blockType == BlockInfo::IfElse) {
            blockInfo.m_jumpToEndBrInfo.push_back({ BlockInfo::JumpToEndBrInfo::IsJump, m_currentFunction->currentByteCodeSize() });
        }
        pushByteCode(Walrus::Jump(offset), WASMOpcode::BrOpcode);

        stopToGenerateByteCodeWhileBlockEnd();
    }

    virtual void OnBrIfExpr(Index depth) override
    {
        if (m_blockInfo.size() == depth) {
            // this case acts like return
            ASSERT(peekVMStackSize() == Walrus::valueSizeInStack(toValueKind(Type::I32)));
            auto stackPos = popVMStack();
            size_t pos = m_currentFunction->currentByteCodeSize();
            pushByteCode(Walrus::JumpIfFalse(stackPos, sizeof(Walrus::JumpIfFalse) + sizeof(Walrus::End) + sizeof(uint16_t) * m_currentFunctionType->result().size()), WASMOpcode::BrIfOpcode);
            for (size_t i = 0; i < m_currentFunctionType->result().size(); i++) {
                ASSERT((m_vmStack.rbegin() + i)->m_size == Walrus::valueSizeInStack(m_currentFunctionType->result()[m_currentFunctionType->result().size() - i - 1]));
            }
            generateEndCode();
            return;
        }

        ASSERT(peekVMStackSize() == Walrus::valueSizeInStack(toValueKind(Type::I32)));
        auto stackPos = popVMStack();

        auto& blockInfo = findBlockInfoInBr(depth);
        auto dropSize = dropStackValuesBeforeBrIfNeeds(depth);
        if (dropSize.second) {
            size_t pos = m_currentFunction->currentByteCodeSize();
            pushByteCode(Walrus::JumpIfFalse(stackPos), WASMOpcode::BrIfOpcode);
            generateMoveValuesCodeRegardToDrop(dropSize);
            auto offset = (int32_t)blockInfo.m_position - (int32_t)m_currentFunction->currentByteCodeSize();
            if (blockInfo.m_blockType == BlockInfo::Block || blockInfo.m_blockType == BlockInfo::IfElse) {
                blockInfo.m_jumpToEndBrInfo.push_back({ BlockInfo::JumpToEndBrInfo::IsJump, m_currentFunction->currentByteCodeSize() });
            }
            pushByteCode(Walrus::Jump(offset), WASMOpcode::BrIfOpcode);
            m_currentFunction->peekByteCode<Walrus::JumpIfFalse>(pos)
                ->setOffset(m_currentFunction->currentByteCodeSize() - pos);
        } else {
            auto offset = (int32_t)blockInfo.m_position - (int32_t)m_currentFunction->currentByteCodeSize();
            if (blockInfo.m_blockType == BlockInfo::Block || blockInfo.m_blockType == BlockInfo::IfElse) {
                blockInfo.m_jumpToEndBrInfo.push_back({ BlockInfo::JumpToEndBrInfo::IsJumpIf, m_currentFunction->currentByteCodeSize() });
            }
            pushByteCode(Walrus::JumpIfTrue(stackPos, offset), WASMOpcode::BrIfOpcode);
        }
    }

    void emitBrTableCase(size_t brTableCode, Index depth, size_t jumpOffset)
    {
        int32_t offset = (int32_t)(m_currentFunction->currentByteCodeSize() - brTableCode);

        if (m_blockInfo.size() == depth) {
            // this case acts like return
#if !defined(NDEBUG)
            for (size_t i = 0; i < m_currentFunctionType->result().size(); i++) {
                ASSERT((m_vmStack.rbegin() + i)->m_size == Walrus::valueSizeInStack(m_currentFunctionType->result()[m_currentFunctionType->result().size() - i - 1]));
            }
#endif
            *(int32_t*)(m_currentFunction->peekByteCode<uint8_t>(brTableCode) + jumpOffset) = offset;
            generateEndCode();
            return;
        }

        auto dropSize = dropStackValuesBeforeBrIfNeeds(depth);

        if (UNLIKELY(dropSize.second)) {
            *(int32_t*)(m_currentFunction->peekByteCode<uint8_t>(brTableCode) + jumpOffset) = offset;
            OnBrExpr(depth);
            return;
        }

        auto& blockInfo = findBlockInfoInBr(depth);

        offset = (int32_t)(blockInfo.m_position - brTableCode);

        if (blockInfo.m_blockType == BlockInfo::Block || blockInfo.m_blockType == BlockInfo::IfElse) {
            offset = jumpOffset;
            blockInfo.m_jumpToEndBrInfo.push_back({ BlockInfo::JumpToEndBrInfo::IsBrTable, brTableCode + jumpOffset });
        }

        *(int32_t*)(m_currentFunction->peekByteCode<uint8_t>(brTableCode) + jumpOffset) = offset;
    }

    virtual void OnBrTableExpr(Index numTargets, Index* targetDepths, Index defaultTargetDepth) override
    {
        ASSERT(peekVMStackSize() == Walrus::valueSizeInStack(toValueKind(Type::I32)));
        auto stackPos = popVMStack();

        size_t brTableCode = m_currentFunction->currentByteCodeSize();
        pushByteCode(Walrus::BrTable(stackPos, numTargets), WASMOpcode::BrTableOpcode);

        if (numTargets) {
            m_currentFunction->expandByteCode(sizeof(int32_t) * numTargets);

            for (Index i = 0; i < numTargets; i++) {
                emitBrTableCase(brTableCode, targetDepths[i], sizeof(Walrus::BrTable) + i * sizeof(int32_t));
            }
        }

        // generate default
        emitBrTableCase(brTableCode, defaultTargetDepth, Walrus::BrTable::offsetOfDefault());
        stopToGenerateByteCodeWhileBlockEnd();
    }

    virtual void OnSelectExpr(Index resultCount, Type* resultTypes) override
    {
        ASSERT(peekVMStackSize() == Walrus::valueSizeInStack(toValueKind(Type::I32)));
        ASSERT(resultCount == 0 || resultCount == 1);
        auto stackPos = popVMStack();

        auto size = peekVMStackSize();
        auto src1 = popVMStack();
        auto src0 = popVMStack();
        auto dst = pushVMStack(size);
        pushByteCode(Walrus::Select(stackPos, size, src0, src1, dst), WASMOpcode::SelectOpcode);
    }

    virtual void OnThrowExpr(Index tagIndex) override
    {
        auto pos = m_currentFunction->currentByteCodeSize();
        uint32_t offsetsSize = 0;

        if (tagIndex != std::numeric_limits<Index>::max()) {
            offsetsSize = m_result.m_functionTypes[m_result.m_tagTypes[tagIndex]->sigIndex()]->param().size();
        }

        pushByteCode(Walrus::Throw(tagIndex, offsetsSize), WASMOpcode::ThrowOpcode);

        if (tagIndex != std::numeric_limits<Index>::max()) {
            auto functionType = m_result.m_functionTypes[m_result.m_tagTypes[tagIndex]->sigIndex()];
            auto& param = functionType->param();
            m_currentFunction->expandByteCode(sizeof(uint16_t) * param.size());
            Walrus::Throw* code = m_currentFunction->peekByteCode<Walrus::Throw>(pos);
            for (size_t i = 0; i < param.size(); i++) {
                code->dataOffsets()[param.size() - i - 1] = (m_vmStack.rbegin() + i)->m_position;
            }
            for (size_t i = 0; i < param.size(); i++) {
                ASSERT(peekVMStackSize() == Walrus::valueSizeInStack(functionType->param()[functionType->param().size() - i - 1]));
                popVMStack();
            }
        }

        stopToGenerateByteCodeWhileBlockEnd();
    }

    virtual void OnTryExpr(Type sigType) override
    {
        BlockInfo b(BlockInfo::TryCatch, sigType, *this);
        m_blockInfo.push_back(b);
    }

    void processCatchExpr(Index tagIndex)
    {
        ASSERT(m_blockInfo.back().m_blockType == BlockInfo::TryCatch);
        keepSubResultsIfNeeds();

        auto& blockInfo = m_blockInfo.back();
        restoreVMStackRegardToPartOfBlockEnd(blockInfo);

        size_t tryEnd = m_currentFunction->currentByteCodeSize();
        if (m_catchInfo.size() && m_catchInfo.back().m_tryCatchBlockDepth == m_blockInfo.size()) {
            // not first catch
            tryEnd = m_catchInfo.back().m_tryEnd;
            blockInfo.m_jumpToEndBrInfo.push_back({ BlockInfo::JumpToEndBrInfo::IsJump, m_currentFunction->currentByteCodeSize() });
            pushByteCode(Walrus::Jump(), WASMOpcode::CatchOpcode);
        } else {
            // first catch
            blockInfo.m_jumpToEndBrInfo.push_back({ BlockInfo::JumpToEndBrInfo::IsJump, m_currentFunction->currentByteCodeSize() });
            pushByteCode(Walrus::Jump(), WASMOpcode::CatchOpcode);
        }

        m_catchInfo.push_back({ m_blockInfo.size(), m_blockInfo.back().m_position, tryEnd, m_currentFunction->currentByteCodeSize(), tagIndex });

        if (tagIndex != std::numeric_limits<Index>::max()) {
            auto functionType = m_result.m_functionTypes[m_result.m_tagTypes[tagIndex]->sigIndex()];
            for (size_t i = 0; i < functionType->param().size(); i++) {
                pushVMStack(Walrus::valueSizeInStack(functionType->param()[i]));
            }
        }
    }

    virtual void OnCatchExpr(Index tagIndex) override
    {
        processCatchExpr(tagIndex);
    }

    virtual void OnCatchAllExpr() override
    {
        processCatchExpr(std::numeric_limits<Index>::max());
    }

    virtual void OnMemoryInitExpr(Index segmentIndex, Index memidx) override
    {
        ASSERT(peekVMStackSize() == Walrus::valueSizeInStack(toValueKind(Type::I32)));
        auto src2 = popVMStack();
        ASSERT(peekVMStackSize() == Walrus::valueSizeInStack(toValueKind(Type::I32)));
        auto src1 = popVMStack();
        ASSERT(peekVMStackSize() == Walrus::valueSizeInStack(toValueKind(Type::I32)));
        auto src0 = popVMStack();

        pushByteCode(Walrus::MemoryInit(memidx, segmentIndex, src0, src1, src2), WASMOpcode::MemoryInitOpcode);
    }

    virtual void OnMemoryCopyExpr(Index srcMemIndex, Index dstMemIndex) override
    {
        ASSERT(peekVMStackSize() == Walrus::valueSizeInStack(toValueKind(Type::I32)));
        auto src2 = popVMStack();
        ASSERT(peekVMStackSize() == Walrus::valueSizeInStack(toValueKind(Type::I32)));
        auto src1 = popVMStack();
        ASSERT(peekVMStackSize() == Walrus::valueSizeInStack(toValueKind(Type::I32)));
        auto src0 = popVMStack();

        pushByteCode(Walrus::MemoryCopy(srcMemIndex, dstMemIndex, src0, src1, src2), WASMOpcode::MemoryCopyOpcode);
    }

    virtual void OnMemoryFillExpr(Index memidx) override
    {
        ASSERT(peekVMStackSize() == Walrus::valueSizeInStack(toValueKind(Type::I32)));
        auto src2 = popVMStack();
        ASSERT(peekVMStackSize() == Walrus::valueSizeInStack(toValueKind(Type::I32)));
        auto src1 = popVMStack();
        ASSERT(peekVMStackSize() == Walrus::valueSizeInStack(toValueKind(Type::I32)));
        auto src0 = popVMStack();

        pushByteCode(Walrus::MemoryFill(memidx, src0, src1, src2), WASMOpcode::MemoryFillOpcode);
    }

    virtual void OnDataDropExpr(Index segmentIndex) override
    {
        pushByteCode(Walrus::DataDrop(segmentIndex), WASMOpcode::DataDropOpcode);
    }

    virtual void OnMemoryGrowExpr(Index memidx) override
    {
        ASSERT(peekVMStackSize() == Walrus::valueSizeInStack(toValueKind(Type::I32)));
        auto src = popVMStack();
        auto dst = pushVMStack(Walrus::valueSizeInStack(Walrus::Value::Type::I32));
        pushByteCode(Walrus::MemoryGrow(memidx, src, dst), WASMOpcode::MemoryGrowOpcode);
    }

    virtual void OnMemorySizeExpr(Index memidx) override
    {
        auto stackPos = pushVMStack(Walrus::valueSizeInStack(Walrus::Value::Type::I32));
        pushByteCode(Walrus::MemorySize(memidx, stackPos), WASMOpcode::MemorySizeOpcode);
    }

    virtual void OnTableGetExpr(Index tableIndex) override
    {
        ASSERT(peekVMStackSize() == Walrus::valueSizeInStack(toValueKind(Type::I32)));
        auto src = popVMStack();
        auto dst = pushVMStack(Walrus::valueSizeInStack(Walrus::Value::Type::FuncRef));
        pushByteCode(Walrus::TableGet(tableIndex, src, dst), WASMOpcode::TableGetOpcode);
    }

    virtual void OnTableSetExpr(Index table_index) override
    {
        ASSERT(peekVMStackSize() == Walrus::valueSizeInStack(toValueKind(Type::FuncRef)));
        auto src1 = popVMStack();
        ASSERT(peekVMStackSize() == Walrus::valueSizeInStack(toValueKind(Type::I32)));
        auto src0 = popVMStack();
        pushByteCode(Walrus::TableSet(table_index, src0, src1), WASMOpcode::TableSetOpcode);
    }

    virtual void OnTableGrowExpr(Index table_index) override
    {
        ASSERT(peekVMStackSize() == Walrus::valueSizeInStack(toValueKind(Type::I32)));
        auto src1 = popVMStack();
        ASSERT(peekVMStackSize() == Walrus::valueSizeInStack(toValueKind(Type::FuncRef)));
        auto src0 = popVMStack();
        auto dst = pushVMStack(Walrus::valueSizeInStack(Walrus::Value::Type::I32));
        pushByteCode(Walrus::TableGrow(table_index, src0, src1, dst), WASMOpcode::TableGrowOpcode);
    }

    virtual void OnTableSizeExpr(Index table_index) override
    {
        auto dst = pushVMStack(Walrus::valueSizeInStack(Walrus::Value::Type::I32));
        pushByteCode(Walrus::TableSize(table_index, dst), WASMOpcode::TableSizeOpcode);
    }

    virtual void OnTableCopyExpr(Index dst_index, Index src_index) override
    {
        ASSERT(peekVMStackSize() == Walrus::valueSizeInStack(toValueKind(Type::I32)));
        auto src2 = popVMStack();
        ASSERT(peekVMStackSize() == Walrus::valueSizeInStack(toValueKind(Type::I32)));
        auto src1 = popVMStack();
        ASSERT(peekVMStackSize() == Walrus::valueSizeInStack(toValueKind(Type::I32)));
        auto src0 = popVMStack();
        pushByteCode(Walrus::TableCopy(dst_index, src_index, src0, src1, src2), WASMOpcode::TableCopyOpcode);
    }

    virtual void OnTableFillExpr(Index table_index) override
    {
        ASSERT(peekVMStackSize() == Walrus::valueSizeInStack(toValueKind(Type::I32)));
        auto src2 = popVMStack();
        ASSERT(peekVMStackSize() == Walrus::valueSizeInStack(toValueKind(Type::FuncRef)));
        auto src1 = popVMStack();
        ASSERT(peekVMStackSize() == Walrus::valueSizeInStack(toValueKind(Type::I32)));
        auto src0 = popVMStack();
        pushByteCode(Walrus::TableFill(table_index, src0, src1, src2), WASMOpcode::TableFillOpcode);
    }

    virtual void OnElemDropExpr(Index segmentIndex) override
    {
        pushByteCode(Walrus::ElemDrop(segmentIndex), WASMOpcode::ElemDropOpcode);
    }

    virtual void OnTableInitExpr(Index segmentIndex, Index tableIndex) override
    {
        ASSERT(peekVMStackSize() == Walrus::valueSizeInStack(toValueKind(Type::I32)));
        auto src2 = popVMStack();
        ASSERT(peekVMStackSize() == Walrus::valueSizeInStack(toValueKind(Type::I32)));
        auto src1 = popVMStack();
        ASSERT(peekVMStackSize() == Walrus::valueSizeInStack(toValueKind(Type::I32)));
        auto src0 = popVMStack();
        pushByteCode(Walrus::TableInit(tableIndex, segmentIndex, src0, src1, src2), WASMOpcode::TableInitOpcode);
    }

    virtual void OnLoadExpr(int opcode, Index memidx, Address alignmentLog2, Address offset) override
    {
        auto code = static_cast<WASMOpcode>(opcode);
        ASSERT(WASMCodeInfo::codeTypeToMemorySize(g_wasmCodeInfo[opcode].m_paramTypes[0]) == peekVMStackSize());
        auto src = popVMStack();
        auto dst = pushVMStack(WASMCodeInfo::codeTypeToMemorySize(g_wasmCodeInfo[opcode].m_resultType));
        if ((opcode == (int)WASMOpcode::I32LoadOpcode || opcode == (int)WASMOpcode::F32LoadOpcode) && offset == 0) {
            pushByteCode(Walrus::Load32(src, dst), code);
        } else if ((opcode == (int)WASMOpcode::I64LoadOpcode || opcode == (int)WASMOpcode::F64LoadOpcode) && offset == 0) {
            pushByteCode(Walrus::Load64(src, dst), code);
        } else {
            generateMemoryLoadCode(code, offset, src, dst);
        }
    }

    virtual void OnStoreExpr(int opcode, Index memidx, Address alignmentLog2, Address offset) override
    {
        auto code = static_cast<WASMOpcode>(opcode);
        ASSERT(WASMCodeInfo::codeTypeToMemorySize(g_wasmCodeInfo[opcode].m_paramTypes[1]) == peekVMStackSize());
        auto src1 = popVMStack();
        ASSERT(WASMCodeInfo::codeTypeToMemorySize(g_wasmCodeInfo[opcode].m_paramTypes[0]) == peekVMStackSize());
        auto src0 = popVMStack();
        if ((opcode == (int)WASMOpcode::I32StoreOpcode || opcode == (int)WASMOpcode::F32StoreOpcode) && offset == 0) {
            pushByteCode(Walrus::Store32(src0, src1), code);
        } else if ((opcode == (int)WASMOpcode::I64StoreOpcode || opcode == (int)WASMOpcode::F64StoreOpcode) && offset == 0) {
            pushByteCode(Walrus::Store64(src0, src1), code);
        } else {
            generateMemoryStoreCode(code, offset, src0, src1);
        }
    }

    virtual void OnRefFuncExpr(Index func_index) override
    {
        auto dst = pushVMStack(Walrus::valueSizeInStack(Walrus::Value::Type::FuncRef));
        pushByteCode(Walrus::RefFunc(func_index, dst), WASMOpcode::RefFuncOpcode);
    }

    virtual void OnRefNullExpr(Type type) override
    {
        ASSERT(sizeof(uintptr_t) == 4 || sizeof(uintptr_t) == 8);
        Walrus::ByteCodeStackOffset dst = pushVMStack(Walrus::valueSizeInStack(Walrus::Value::Type::FuncRef));

        if (sizeof(uintptr_t) == 4) {
            pushByteCode(Walrus::Const32(dst, Walrus::Value::Null), WASMOpcode::Const32Opcode);
        } else {
            pushByteCode(Walrus::Const64(dst, Walrus::Value::Null), WASMOpcode::Const64Opcode);
        }
    }

    virtual void OnRefIsNullExpr() override
    {
        ASSERT(sizeof(uintptr_t) == 4 || sizeof(uintptr_t) == 8);
        auto src = popVMStack();
        auto dst = pushVMStack(Walrus::valueSizeInStack(Walrus::Value::Type::I32));
        if (sizeof(uintptr_t) == 4) {
            pushByteCode(Walrus::I32Eqz(src, dst), WASMOpcode::RefIsNullOpcode);
        } else {
            pushByteCode(Walrus::I64Eqz(src, dst), WASMOpcode::RefIsNullOpcode);
        }
    }

    virtual void OnNopExpr() override
    {
    }

    virtual void OnReturnExpr() override
    {
        generateFunctionReturnCode();
    }

    virtual void OnEndExpr() override
    {
        if (m_blockInfo.size()) {
            auto dropSize = dropStackValuesBeforeBrIfNeeds(0);
            auto blockInfo = m_blockInfo.back();
            m_blockInfo.pop_back();

#if !defined(NDEBUG)
            if (!blockInfo.m_shouldRestoreVMStackAtEnd) {
                if (!blockInfo.m_returnValueType.IsIndex() && blockInfo.m_returnValueType != Type::Void) {
                    ASSERT(peekVMStackSize() == Walrus::valueSizeInStack(toValueKind(blockInfo.m_returnValueType)));
                }
            }
#endif

            switch (blockInfo.m_blockType) {
            case BlockInfo::TryCatch: {
                auto iter = m_catchInfo.begin();
                while (iter != m_catchInfo.end()) {
                    if (iter->m_tryCatchBlockDepth - 1 != m_blockInfo.size()) {
                        iter++;
                        continue;
                    }
                    size_t stackSizeToBe = m_initialFunctionStackSize;
                    for (size_t i = 0; i < blockInfo.m_vmStack.size(); i++) {
                        stackSizeToBe += m_vmStack[i].m_size;
                    }
                    m_currentFunction->m_catchInfo.push_back({ iter->m_tryStart, iter->m_tryEnd, iter->m_catchStart, stackSizeToBe, iter->m_tagIndex });
                    iter = m_catchInfo.erase(iter);
                }
                break;
            }
            case BlockInfo::Loop:
            case BlockInfo::Block: {
                if (blockInfo.m_byteCodeGenerationStopped && blockInfo.m_jumpToEndBrInfo.size() == 0) {
                    stopToGenerateByteCodeWhileBlockEnd();
                    return;
                }
                break;
            }
            default: {
                break;
            }
            }

            if (blockInfo.m_shouldRestoreVMStackAtEnd) {
                if (dropSize.second) {
                    generateMoveValuesCodeRegardToDrop(dropSize);
                }
                restoreVMStackBy(blockInfo);
                if (blockInfo.m_returnValueType.IsIndex()) {
                    auto ft = m_result.m_functionTypes[blockInfo.m_returnValueType];
                    const auto& param = ft->param();
                    for (size_t i = 0; i < param.size(); i++) {
                        ASSERT(peekVMStackSize() == Walrus::valueSizeInStack(param[param.size() - i - 1]));
                        popVMStack();
                    }

                    const auto& result = ft->result();
                    for (size_t i = 0; i < result.size(); i++) {
                        pushVMStack(Walrus::valueSizeInStack(result[i]));
                    }
                } else if (blockInfo.m_returnValueType != Type::Void) {
                    pushVMStack(Walrus::valueSizeInStack(toValueKind(blockInfo.m_returnValueType)));
                }
            }

            for (size_t i = 0; i < blockInfo.m_jumpToEndBrInfo.size(); i++) {
                switch (blockInfo.m_jumpToEndBrInfo[i].m_type) {
                case BlockInfo::JumpToEndBrInfo::IsJump:
                    m_currentFunction->peekByteCode<Walrus::Jump>(blockInfo.m_jumpToEndBrInfo[i].m_position)->setOffset(m_currentFunction->currentByteCodeSize() - blockInfo.m_jumpToEndBrInfo[i].m_position);
                    break;
                case BlockInfo::JumpToEndBrInfo::IsJumpIf:
                    m_currentFunction->peekByteCode<Walrus::JumpIfFalse>(blockInfo.m_jumpToEndBrInfo[i].m_position)
                        ->setOffset(m_currentFunction->currentByteCodeSize() - blockInfo.m_jumpToEndBrInfo[i].m_position);
                    break;
                default:
                    ASSERT(blockInfo.m_jumpToEndBrInfo[i].m_type == BlockInfo::JumpToEndBrInfo::IsBrTable);

                    int32_t* offset = m_currentFunction->peekByteCode<int32_t>(blockInfo.m_jumpToEndBrInfo[i].m_position);
                    *offset = m_currentFunction->currentByteCodeSize() + (size_t)*offset - blockInfo.m_jumpToEndBrInfo[i].m_position;
                    break;
                }
            }
        } else {
            generateEndCode();
        }
    }

    virtual void OnUnreachableExpr() override
    {
        pushByteCode(Walrus::Unreachable(), WASMOpcode::UnreachableOpcode);
        stopToGenerateByteCodeWhileBlockEnd();
    }

    virtual void EndFunctionBody(Index index) override
    {
#if !defined(NDEBUG)
        if (getenv("DUMP_BYTECODE") && strlen(getenv("DUMP_BYTECODE"))) {
            m_currentFunction->dumpByteCode();
        }
        if (m_shouldContinueToGenerateByteCode) {
            for (size_t i = 0; i < m_currentFunctionType->result().size() && m_vmStack.size(); i++) {
                ASSERT(popVMStackSize() == Walrus::valueSizeInStack(m_currentFunctionType->result()[m_currentFunctionType->result().size() - i - 1]));
            }
            ASSERT(m_vmStack.empty());
        }
#endif

        ASSERT(m_currentFunction == m_result.m_functions[index]);
        endFunction();
    }

    void generateBinaryCode(WASMOpcode code, size_t src0, size_t src1, size_t dst)
    {
        switch (code) {
#define GENERATE_BINARY_CODE_CASE(name, op, paramType, returnType) \
    case WASMOpcode::name##Opcode: {                               \
        pushByteCode(Walrus::name(src0, src1, dst), code);         \
        break;                                                     \
    }
            FOR_EACH_BYTECODE_BINARY_OP(GENERATE_BINARY_CODE_CASE)
#undef GENERATE_BINARY_CODE_CASE
        default:
            ASSERT_NOT_REACHED();
            break;
        }
    }

    void generateUnaryCode(WASMOpcode code, size_t src, size_t dst)
    {
        switch (code) {
#define GENERATE_UNARY_CODE_CASE(name, ...)         \
    case WASMOpcode::name##Opcode: {                \
        pushByteCode(Walrus::name(src, dst), code); \
        break;                                      \
    }
            FOR_EACH_BYTECODE_UNARY_OP(GENERATE_UNARY_CODE_CASE)
            FOR_EACH_BYTECODE_UNARY_OP_2(GENERATE_UNARY_CODE_CASE)
#undef GENERATE_UNARY_CODE_CASE
        default:
            ASSERT_NOT_REACHED();
            break;
        }
    }

    void generateMemoryLoadCode(WASMOpcode code, size_t offset, size_t src, size_t dst)
    {
        switch (code) {
#define GENERATE_LOAD_CODE_CASE(name, readType, writeType)  \
    case WASMOpcode::name##Opcode: {                        \
        pushByteCode(Walrus::name(offset, src, dst), code); \
        break;                                              \
    }
            FOR_EACH_BYTECODE_LOAD_OP(GENERATE_LOAD_CODE_CASE)
#undef GENERATE_LOAD_CODE_CASE
        default:
            ASSERT_NOT_REACHED();
            break;
        }
    }

    void generateMemoryStoreCode(WASMOpcode code, size_t offset, size_t src0, size_t src1)
    {
        switch (code) {
#define GENERATE_STORE_CODE_CASE(name, readType, writeType)   \
    case WASMOpcode::name##Opcode: {                          \
        pushByteCode(Walrus::name(offset, src0, src1), code); \
        break;                                                \
    }
            FOR_EACH_BYTECODE_STORE_OP(GENERATE_STORE_CODE_CASE)
#undef GENERATE_STORE_CODE_CASE
        default:
            ASSERT_NOT_REACHED();
            break;
        }
    }

    bool isBinaryOperation(WASMOpcode opcode)
    {
        switch (opcode) {
#define GENERATE_BINARY_CODE_CASE(name, op, paramType, returnType) \
    case WASMOpcode::name##Opcode:
            FOR_EACH_BYTECODE_BINARY_OP(GENERATE_BINARY_CODE_CASE)
#undef GENERATE_BINARY_CODE_CASE
            return true;
        default:
            return false;
        }
    }

    Walrus::WASMParsingResult& parsingResult() { return m_result; }
};

} // namespace wabt

namespace Walrus {

WASMParsingResult::WASMParsingResult()
    : m_seenStartAttribute(false)
    , m_version(0)
    , m_start(0)
{
}

void WASMParsingResult::clear()
{
    for (size_t i = 0; i < m_imports.size(); i++) {
        delete m_imports[i];
    }

    for (size_t i = 0; i < m_exports.size(); i++) {
        delete m_exports[i];
    }

    for (size_t i = 0; i < m_functions.size(); i++) {
        delete m_functions[i];
    }

    for (size_t i = 0; i < m_datas.size(); i++) {
        delete m_datas[i];
    }

    for (size_t i = 0; i < m_elements.size(); i++) {
        delete m_elements[i];
    }

    for (size_t i = 0; i < m_functionTypes.size(); i++) {
        delete m_functionTypes[i];
    }

    for (size_t i = 0; i < m_globalTypes.size(); i++) {
        delete m_globalTypes[i];
    }

    for (size_t i = 0; i < m_tableTypes.size(); i++) {
        delete m_tableTypes[i];
    }

    for (size_t i = 0; i < m_memoryTypes.size(); i++) {
        delete m_memoryTypes[i];
    }

    for (size_t i = 0; i < m_tagTypes.size(); i++) {
        delete m_tagTypes[i];
    }
}

std::pair<Optional<Module*>, std::string> WASMParser::parseBinary(Store* store, const std::string& filename, const uint8_t* data, size_t len)
{
    wabt::WASMBinaryReader delegate;

    std::string error = ReadWasmBinary(filename, data, len, &delegate);
    if (error.length()) {
        return std::make_pair(nullptr, error);
    }

    Module* module = new Module(store, delegate.parsingResult());
    return std::make_pair(module, std::string());
}

} // namespace Walrus
