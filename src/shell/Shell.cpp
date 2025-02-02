#include <iostream>
#include <locale>
#include <sstream>
#include <iomanip>
#include <inttypes.h>

#include "Walrus.h"
#include "runtime/Engine.h"
#include "runtime/Store.h"
#include "runtime/Module.h"
#include "runtime/Instance.h"
#include "runtime/Function.h"
#include "runtime/Table.h"
#include "runtime/Memory.h"
#include "runtime/Global.h"
#include "runtime/Tag.h"
#include "runtime/Trap.h"
#include "parser/WASMParser.h"

#include "wabt/wast-lexer.h"
#include "wabt/wast-parser.h"
#include "wabt/binary-writer.h"

struct spectestseps : std::numpunct<char> {
    char do_thousands_sep() const { return '_'; }
    std::string do_grouping() const { return "\3"; }
};

using namespace Walrus;

static void printI32(int32_t v)
{
    std::stringstream ss;
    std::locale slocale(std::locale(), new spectestseps);
    ss.imbue(slocale);
    ss << v;
    printf("%s : i32\n", ss.str().c_str());
}

static void printI64(int64_t v)
{
    std::stringstream ss;
    std::locale slocale(std::locale(), new spectestseps);
    ss.imbue(slocale);
    ss << v;
    printf("%s : i64\n", ss.str().c_str());
}

static std::string formatDecmialString(std::string s)
{
    while (s.find('.') != std::string::npos && s[s.length() - 1] == '0') {
        s.resize(s.length() - 1);
    }

    if (s.length() && s[s.length() - 1] == '.') {
        s.resize(s.length() - 1);
    }

    auto pos = s.find('.');
    if (pos != std::string::npos) {
        std::string out = s.substr(0, pos);
        out += ".";

        size_t cnt = 0;
        for (size_t i = pos + 1; i < s.length(); i++) {
            out += s[i];
            cnt++;
            if (cnt % 3 == 0 && i != s.length() - 1) {
                out += "_";
            }
        }

        s = out;
    }

    return s;
}

static void printF32(float v)
{
    std::stringstream ss;
    ss.imbue(std::locale(std::locale(), new spectestseps));
    ss.setf(std::ios_base::fixed);
    ss << std::setprecision(std::numeric_limits<float>::max_digits10);
    ss << v;
    printf("%s : f32\n", formatDecmialString(ss.str()).c_str());
}

static void printF64(double v)
{
    std::stringstream ss;
    ss.imbue(std::locale(std::locale(), new spectestseps));
    ss.setf(std::ios_base::fixed);
    ss << std::setprecision(std::numeric_limits<double>::max_digits10 - 1);
    ss << v;
    printf("%s : f64\n", formatDecmialString(ss.str()).c_str());
}

class SpecTestFunctionTypes {
    MAKE_STACK_ALLOCATED();

public:
    enum Index : uint8_t {
        NONE = 0,
        I32,
        I64,
        F32,
        F64,
        I32F32,
        F64F64,
        INVALID,
        INDEX_NUM,
    };

    SpecTestFunctionTypes()
    {
        m_vector.reserve(INDEX_NUM);
        size_t index = 0;

        ValueTypeVector* param;
        ValueTypeVector* result;

        {
            // NONE
            param = new ValueTypeVector();
            result = new ValueTypeVector();
            m_vector[index++] = new FunctionType(param, result);
        }
        {
            // I32
            param = new ValueTypeVector();
            result = new ValueTypeVector();
            param->push_back(Value::Type::I32);
            m_vector[index++] = new FunctionType(param, result);
        }
        {
            // I64
            param = new ValueTypeVector();
            result = new ValueTypeVector();
            param->push_back(Value::Type::I64);
            m_vector[index++] = new FunctionType(param, result);
        }
        {
            // F32
            param = new ValueTypeVector();
            result = new ValueTypeVector();
            param->push_back(Value::Type::F32);
            m_vector[index++] = new FunctionType(param, result);
        }
        {
            // F64
            param = new ValueTypeVector();
            result = new ValueTypeVector();
            param->push_back(Value::Type::F64);
            m_vector[index++] = new FunctionType(param, result);
        }
        {
            // I32F32
            param = new ValueTypeVector();
            result = new ValueTypeVector();
            param->push_back(Value::Type::I32);
            param->push_back(Value::Type::F32);
            m_vector[index++] = new FunctionType(param, result);
        }
        {
            // F64F64
            param = new ValueTypeVector();
            result = new ValueTypeVector();
            param->push_back(Value::Type::F64);
            param->push_back(Value::Type::F64);
            m_vector[index++] = new FunctionType(param, result);
        }
        {
            // INVALID
            param = new ValueTypeVector();
            result = new ValueTypeVector();
            param->push_back(Value::Type::Void);
            m_vector[index++] = new FunctionType(param, result);
        }

        ASSERT(index == INDEX_NUM);
    }

    ~SpecTestFunctionTypes()
    {
        for (size_t i = 0; i < m_vector.size(); i++) {
            delete m_vector[i];
        }
    }

    FunctionType* operator[](const size_t idx)
    {
        ASSERT(idx < m_vector.size());
        return m_vector[idx];
    }

private:
    FunctionTypeVector m_vector;
};

static Trap::TrapResult executeWASM(Store* store, const std::string& filename, const std::vector<uint8_t>& src, SpecTestFunctionTypes& functionTypes,
                                    std::map<std::string, Instance*>* registeredInstanceMap = nullptr)
{
    auto parseResult = WASMParser::parseBinary(store, filename, src.data(), src.size());
    if (!parseResult.second.empty()) {
        Trap::TrapResult tr;
        tr.exception = Exception::create(parseResult.second);
        return tr;
    }
    auto module = parseResult.first;
    const auto& importTypes = module->imports();

    ExternVector importValues;
    importValues.reserve(importTypes.size());
    /*
        (module ;; spectest host module(https://github.com/WebAssembly/spec/tree/main/interpreter)
          (global (export "global_i32") i32)
          (global (export "global_i64") i64)
          (global (export "global_f32") f32)
          (global (export "global_f64") f64)

          (table (export "table") 10 20 funcref)

          (memory (export "memory") 1 2)

          (func (export "print"))
          (func (export "print_i32") (param i32))
          (func (export "print_i64") (param i64))
          (func (export "print_f32") (param f32))
          (func (export "print_f64") (param f64))
          (func (export "print_i32_f32") (param i32 f32))
          (func (export "print_f64_f64") (param f64 f64))
        )
    */

    for (size_t i = 0; i < importTypes.size(); i++) {
        auto import = importTypes[i];
        if (import->moduleName() == "spectest") {
            if (import->fieldName() == "print") {
                auto ft = functionTypes[SpecTestFunctionTypes::NONE];
                importValues.push_back(ImportedFunction::createImportedFunction(
                    store,
                    ft,
                    [](ExecutionState& state, const uint32_t argc, Value* argv, Value* result, void* data) {
                    },
                    nullptr));
            } else if (import->fieldName() == "print_i32") {
                auto ft = functionTypes[SpecTestFunctionTypes::I32];
                importValues.push_back(ImportedFunction::createImportedFunction(
                    store,
                    ft,
                    [](ExecutionState& state, const uint32_t argc, Value* argv, Value* result, void* data) {
                        printI32(argv[0].asI32());
                    },
                    nullptr));
            } else if (import->fieldName() == "print_i64") {
                auto ft = functionTypes[SpecTestFunctionTypes::I64];
                importValues.push_back(ImportedFunction::createImportedFunction(
                    store,
                    ft,
                    [](ExecutionState& state, const uint32_t argc, Value* argv, Value* result, void* data) {
                        printI64(argv[0].asI64());
                    },
                    nullptr));
            } else if (import->fieldName() == "print_f32") {
                auto ft = functionTypes[SpecTestFunctionTypes::F32];
                importValues.push_back(ImportedFunction::createImportedFunction(
                    store,
                    ft,
                    [](ExecutionState& state, const uint32_t argc, Value* argv, Value* result, void* data) {
                        printF32(argv[0].asF32());
                    },
                    nullptr));
            } else if (import->fieldName() == "print_f64") {
                auto ft = functionTypes[SpecTestFunctionTypes::F64];
                importValues.push_back(ImportedFunction::createImportedFunction(
                    store,
                    ft,
                    [](ExecutionState& state, const uint32_t argc, Value* argv, Value* result, void* data) {
                        printF64(argv[0].asF64());
                    },
                    nullptr));
            } else if (import->fieldName() == "print_i32_f32") {
                auto ft = functionTypes[SpecTestFunctionTypes::I32F32];
                importValues.push_back(ImportedFunction::createImportedFunction(
                    store,
                    ft,
                    [](ExecutionState& state, const uint32_t argc, Value* argv, Value* result, void* data) {
                        printI32(argv[0].asI32());
                        printF32(argv[1].asF32());
                    },
                    nullptr));
            } else if (import->fieldName() == "print_f64_f64") {
                auto ft = functionTypes[SpecTestFunctionTypes::F64F64];
                importValues.push_back(ImportedFunction::createImportedFunction(
                    store,
                    ft,
                    [](ExecutionState& state, const uint32_t argc, Value* argv, Value* result, void* data) {
                        printF64(argv[0].asF64());
                        printF64(argv[1].asF64());
                    },
                    nullptr));
            } else if (import->fieldName() == "global_i32") {
                importValues.push_back(Global::createGlobal(store, Value(int32_t(666))));
            } else if (import->fieldName() == "global_i64") {
                importValues.push_back(Global::createGlobal(store, Value(int64_t(666))));
            } else if (import->fieldName() == "global_f32") {
                importValues.push_back(Global::createGlobal(store, Value(float(0x44268000))));
            } else if (import->fieldName() == "global_f64") {
                importValues.push_back(Global::createGlobal(store, Value(double(0x4084d00000000000))));
            } else if (import->fieldName() == "table") {
                importValues.push_back(Table::createTable(store, Value::Type::FuncRef, 10, 20));
            } else if (import->fieldName() == "memory") {
                importValues.push_back(Memory::createMemory(store, 1 * Memory::s_memoryPageSize, 2 * Memory::s_memoryPageSize));
            } else {
                // import wrong value for test
                auto ft = functionTypes[SpecTestFunctionTypes::INVALID];
                importValues.push_back(ImportedFunction::createImportedFunction(
                    store,
                    ft,
                    [](ExecutionState& state, const uint32_t argc, Value* argv, Value* result, void* data) {
                    },
                    nullptr));
            }
        } else if (import->moduleName() == "wasi_snapshot_preview1") {
            // TODO wasi
            if (import->fieldName() == "proc_exit") {
                auto ft = functionTypes[SpecTestFunctionTypes::I32];
                importValues.push_back(ImportedFunction::createImportedFunction(
                    store,
                    ft,
                    [](ExecutionState& state, const uint32_t argc, Value* argv, Value* result, void* data) {
                        ASSERT(argc == 1 && argv[0].type() == Value::I32);
                        exit(argv[0].asI32());
                        ASSERT_NOT_REACHED();
                    },
                    nullptr));
            }
        } else if (registeredInstanceMap) {
            auto iter = registeredInstanceMap->find(import->moduleName());
            if (iter != registeredInstanceMap->end()) {
                Instance* instance = iter->second;
                auto e = instance->resolveExportType(import->fieldName());
                RELEASE_ASSERT(e);
                if (e->exportType() == ExportType::Function) {
                    importValues.push_back(instance->resolveExportFunction(import->fieldName()));
                } else if (e->exportType() == ExportType::Tag) {
                    importValues.push_back(instance->resolveExportTag(import->fieldName()));
                } else if (e->exportType() == ExportType::Table) {
                    importValues.push_back(instance->resolveExportTable(import->fieldName()));
                } else if (e->exportType() == ExportType::Memory) {
                    importValues.push_back(instance->resolveExportMemory(import->fieldName()));
                } else if (e->exportType() == ExportType::Global) {
                    importValues.push_back(instance->resolveExportGlobal(import->fieldName()));
                } else {
                    RELEASE_ASSERT_NOT_REACHED();
                }
            }
        }
    }

    struct RunData {
        Module* module;
        ExternVector& importValues;
    } data = { module.value(), importValues };
    Walrus::Trap trap;
    return trap.run([](ExecutionState& state, void* d) {
        RunData* data = reinterpret_cast<RunData*>(d);
        data->module->instantiate(state, data->importValues);
    },
                    &data);
}

static bool endsWith(const std::string& str, const std::string& suffix)
{
    return str.size() >= suffix.size() && 0 == str.compare(str.size() - suffix.size(), suffix.size(), suffix);
}

static Walrus::Value toWalrusValue(wabt::Const& c)
{
    switch (c.type()) {
    case wabt::Type::I32:
        return Walrus::Value(static_cast<int32_t>(c.u32()));
    case wabt::Type::I64:
        return Walrus::Value(static_cast<int64_t>(c.u64()));
    case wabt::Type::F32: {
        if (c.is_expected_nan(0)) {
            return Walrus::Value(std::numeric_limits<float>::quiet_NaN());
        }
        float s;
        auto bits = c.f32_bits();
        memcpy(&s, &bits, sizeof(float));
        return Walrus::Value(s);
    }
    case wabt::Type::F64: {
        if (c.is_expected_nan(0)) {
            return Walrus::Value(std::numeric_limits<double>::quiet_NaN());
        }
        double s;
        auto bits = c.f64_bits();
        memcpy(&s, &bits, sizeof(double));
        return Walrus::Value(s);
    }
    case wabt::Type::FuncRef: {
        if (c.ref_bits() == wabt::Const::kRefNullBits) {
            return Walrus::Value(Walrus::Value::FuncRef, Walrus::Value::Null);
        }
        // Add one similar to wabt interpreter.
        return Walrus::Value(Walrus::Value::FuncRef, c.ref_bits() + 1, Walrus::Value::Force);
    }
    case wabt::Type::ExternRef: {
        if (c.ref_bits() == wabt::Const::kRefNullBits) {
            return Walrus::Value(Walrus::Value::ExternRef, Walrus::Value::Null);
        }
        // Add one similar to wabt interpreter.
        return Walrus::Value(Walrus::Value::ExternRef, c.ref_bits() + 1, Walrus::Value::Force);
    }
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static bool isCanonicalNan(float val)
{
    uint32_t s;
    memcpy(&s, &val, sizeof(float));
    return s == 0x7fc00000U || s == 0xffc00000U;
}

static bool isCanonicalNan(double val)
{
    uint64_t s;
    memcpy(&s, &val, sizeof(double));
    return s == 0x7ff8000000000000ULL || s == 0xfff8000000000000ULL;
}

static bool isArithmeticNan(float val)
{
    uint32_t s;
    memcpy(&s, &val, sizeof(float));
    return (s & 0x7fc00000U) == 0x7fc00000U;
}

static bool isArithmeticNan(double val)
{
    uint64_t s;
    memcpy(&s, &val, sizeof(double));
    return (s & 0x7ff8000000000000ULL) == 0x7ff8000000000000ULL;
}

static bool equals(Walrus::Value& v, wabt::Const& c)
{
    if (c.type() == wabt::Type::I32 && v.type() == Walrus::Value::I32) {
        return v.asI32() == static_cast<int32_t>(c.u32());
    } else if (c.type() == wabt::Type::I64 && v.type() == Walrus::Value::I64) {
        return v.asI64() == static_cast<int64_t>(c.u64());
    } else if (c.type() == wabt::Type::F32 && v.type() == Walrus::Value::F32) {
        if (c.is_expected_nan(0)) {
            if (c.expected_nan() == wabt::ExpectedNan::Arithmetic) {
                return isArithmeticNan(v.asF32());
            } else {
                return isCanonicalNan(v.asF32());
            }
        }
        return c.f32_bits() == v.asF32Bits();
    } else if (c.type() == wabt::Type::F64 && v.type() == Walrus::Value::F64) {
        if (c.is_expected_nan(0)) {
            if (c.expected_nan() == wabt::ExpectedNan::Arithmetic) {
                return isArithmeticNan(v.asF64());
            } else {
                return isCanonicalNan(v.asF64());
            }
        }
        return c.f64_bits() == v.asF64Bits();
    } else if (c.type() == wabt::Type::ExternRef && v.type() == Walrus::Value::ExternRef) {
        // FIXME value of c.ref_bits() for RefNull
        wabt::Const constNull;
        constNull.set_null(c.type());
        if (c.ref_bits() == constNull.ref_bits()) {
            // check RefNull
            return v.isNull();
        }
        // Add one similar to wabt interpreter.
        return (c.ref_bits() + 1) == reinterpret_cast<uintptr_t>(v.asExternal());
    } else if (c.type() == wabt::Type::FuncRef && v.type() == Walrus::Value::FuncRef) {
        // FIXME value of c.ref_bits() for RefNull
        wabt::Const constNull;
        constNull.set_null(c.type());
        if (c.ref_bits() == constNull.ref_bits()) {
            // check RefNull
            return v.isNull();
        }
        // Add one similar to wabt interpreter.
        return (c.ref_bits() + 1) == reinterpret_cast<uintptr_t>(v.asFunction());
    }
    return false;
}

static void printConstVector(wabt::ConstVector& v)
{
    for (size_t i = 0; i < v.size(); i++) {
        auto c = v[i];
        if (c.type() == wabt::Type::I32) {
            printf("%" PRIu32, c.u32());
        } else if (c.type() == wabt::Type::I64) {
            printf("%" PRIu64, c.u64());
        } else if (c.type() == wabt::Type::F32) {
            if (c.is_expected_nan(0)) {
                printf("nan");
                return;
            }
            float s;
            auto bits = c.f32_bits();
            memcpy(&s, &bits, sizeof(float));
            printf("%f", s);
        } else if (c.type() == wabt::Type::F64) {
            if (c.is_expected_nan(0)) {
                printf("nan");
                return;
            }
            double s;
            auto bits = c.f64_bits();
            memcpy(&s, &bits, sizeof(double));
            printf("%lf", s);
        } else if (c.type() == wabt::Type::ExternRef) {
            // FIXME value of c.ref_bits() for RefNull
            wabt::Const constNull;
            constNull.set_null(c.type());
            if (c.ref_bits() == constNull.ref_bits()) {
                printf("ref.null");
                return;
            }
        } else if (c.type() == wabt::Type::FuncRef) {
            // FIXME value of c.ref_bits() for RefNull
            wabt::Const constNull;
            constNull.set_null(c.type());
            if (c.ref_bits() == constNull.ref_bits()) {
                printf("ref.null");
                return;
            }
        } else {
            RELEASE_ASSERT_NOT_REACHED();
        }
        if (i + 1 != v.size()) {
            printf(", ");
        }
    }
}

static void executeInvokeAction(wabt::InvokeAction* action, Walrus::Function* fn, wabt::ConstVector expectedResult,
                                const char* expectedException, bool expectUserException = false)
{
    RELEASE_ASSERT(fn->functionType()->param().size() == action->args.size());
    Walrus::ValueVector args;
    for (auto& a : action->args) {
        args.push_back(toWalrusValue(a));
    }

    struct RunData {
        Walrus::Function* fn;
        wabt::ConstVector& expectedResult;
        Walrus::ValueVector& args;
    } data = { fn, expectedResult, args };
    Walrus::Trap trap;
    auto trapResult = trap.run([](Walrus::ExecutionState& state, void* d) {
        RunData* data = reinterpret_cast<RunData*>(d);
        Walrus::ValueVector result;
        result.resize(data->expectedResult.size());
        data->fn->call(state, data->args.size(), data->args.data(), result.data());
        if (data->expectedResult.size()) {
            RELEASE_ASSERT(data->fn->functionType()->result().size() == data->expectedResult.size());
            // compare result
            for (size_t i = 0; i < result.size(); i++) {
                RELEASE_ASSERT(equals(result[i], data->expectedResult[i]));
            }
        }
    },
                               &data);
    if (expectedResult.size()) {
        RELEASE_ASSERT(!trapResult.exception);
    }
    if (expectedException) {
        RELEASE_ASSERT(trapResult.exception);
        std::string& s = trapResult.exception->message();
        RELEASE_ASSERT(s.find(expectedException) == 0);
        printf("invoke %s(", action->name.data());
        printConstVector(action->args);
        printf("), expect exception: %s (line: %d) : OK\n", expectedException, action->loc.line);
    } else if (expectUserException) {
        RELEASE_ASSERT(trapResult.exception->tag());
        printf("invoke %s(", action->name.data());
        printConstVector(action->args);
        printf(") expect user exception() (line: %d) : OK\n", action->loc.line);
    } else if (expectedResult.size()) {
        printf("invoke %s(", action->name.data());
        printConstVector(action->args);
        printf(") expect value(");
        printConstVector(expectedResult);
        printf(") (line: %d) : OK\n", action->loc.line);
    }
}

static std::unique_ptr<wabt::OutputBuffer> readModuleData(wabt::Module* module)
{
    wabt::MemoryStream stream;
    wabt::WriteBinaryOptions options;
    wabt::Features features;
    features.EnableAll();
    options.features = features;
    wabt::WriteBinaryModule(&stream, module, options);
    stream.Flush();
    return stream.ReleaseOutputBuffer();
}

static Instance* fetchInstance(wabt::Var& moduleVar, std::map<size_t, Instance*>& instanceMap,
                               std::map<std::string, Instance*>& registeredInstanceMap)
{
    if (moduleVar.is_index()) {
        return instanceMap[moduleVar.index()];
    }
    return registeredInstanceMap[moduleVar.name()];
}

static void executeWAST(Store* store, const std::string& filename, const std::vector<uint8_t>& src, SpecTestFunctionTypes& functionTypes)
{
    auto lexer = wabt::WastLexer::CreateBufferLexer("test.wabt", src.data(), src.size());
    if (!lexer) {
        RELEASE_ASSERT_NOT_REACHED();
        return;
    }

    wabt::Errors errors;
    std::unique_ptr<wabt::Script> script;
    wabt::Features features;
    features.EnableAll();
    wabt::WastParseOptions parse_wast_options(features);
    auto result = wabt::ParseWastScript(lexer.get(), &script, &errors, &parse_wast_options);
    if (!wabt::Succeeded(result)) {
        RELEASE_ASSERT_NOT_REACHED();
        return;
    }

    std::map<size_t, Instance*> instanceMap;
    std::map<std::string, Instance*> registeredInstanceMap;
    size_t commandCount = 0;
    for (const std::unique_ptr<wabt::Command>& command : script->commands) {
        switch (command->type) {
        case wabt::CommandType::Module:
        case wabt::CommandType::ScriptModule: {
            auto* moduleCommand = static_cast<wabt::ModuleCommand*>(command.get());
            auto buf = readModuleData(&moduleCommand->module);
            executeWASM(store, filename, buf->data, functionTypes, &registeredInstanceMap);
            instanceMap[commandCount] = store->getLastInstance();
            if (moduleCommand->module.name.size()) {
                registeredInstanceMap[moduleCommand->module.name] = store->getLastInstance();
            }
            break;
        }
        case wabt::CommandType::AssertReturn: {
            auto* assertReturn = static_cast<wabt::AssertReturnCommand*>(command.get());
            auto value = fetchInstance(assertReturn->action->module_var, instanceMap, registeredInstanceMap)->resolveExportType(assertReturn->action->name);
            RELEASE_ASSERT(value);
            if (assertReturn->action->type() == wabt::ActionType::Invoke) {
                auto action = static_cast<wabt::InvokeAction*>(assertReturn->action.get());
                auto fn = fetchInstance(action->module_var, instanceMap, registeredInstanceMap)->resolveExportFunction(action->name);
                executeInvokeAction(action, fn, assertReturn->expected, nullptr);
            } else if (assertReturn->action->type() == wabt::ActionType::Get) {
                auto action = static_cast<wabt::GetAction*>(assertReturn->action.get());
                auto v = fetchInstance(action->module_var, instanceMap, registeredInstanceMap)->resolveExportGlobal(action->name)->value();
                RELEASE_ASSERT(equals(v, assertReturn->expected[0]))
                printf("get %s", action->name.data());
                printf(" expect value(");
                printConstVector(assertReturn->expected);
                printf(") (line: %d) : OK\n", action->loc.line);
            } else {
                ASSERT_NOT_REACHED();
            }
            break;
        }
        case wabt::CommandType::AssertTrap: {
            auto* assertTrap = static_cast<wabt::AssertTrapCommand*>(command.get());
            auto value = fetchInstance(assertTrap->action->module_var, instanceMap, registeredInstanceMap)->resolveExportFunction(assertTrap->action->name);
            RELEASE_ASSERT(value);
            if (assertTrap->action->type() == wabt::ActionType::Invoke) {
                auto action = static_cast<wabt::InvokeAction*>(assertTrap->action.get());
                auto fn = fetchInstance(action->module_var, instanceMap, registeredInstanceMap)->resolveExportFunction(action->name);
                executeInvokeAction(action, fn, wabt::ConstVector(), assertTrap->text.data());
            } else {
                ASSERT_NOT_REACHED();
            }
            break;
        }
        case wabt::CommandType::AssertException: {
            auto* assertException = static_cast<wabt::AssertExceptionCommand*>(command.get());
            auto value = fetchInstance(assertException->action->module_var, instanceMap, registeredInstanceMap)->resolveExportFunction(assertException->action->name);
            RELEASE_ASSERT(value);
            if (assertException->action->type() == wabt::ActionType::Invoke) {
                auto action = static_cast<wabt::InvokeAction*>(assertException->action.get());
                auto fn = fetchInstance(action->module_var, instanceMap, registeredInstanceMap)->resolveExportFunction(action->name);
                executeInvokeAction(action, fn, wabt::ConstVector(), nullptr, true);
            } else {
                ASSERT_NOT_REACHED();
            }
            break;
        }
        case wabt::CommandType::AssertUninstantiable: {
            auto* assertModuleUninstantiable = static_cast<wabt::AssertModuleCommand<wabt::CommandType::AssertUninstantiable>*>(command.get());
            auto m = assertModuleUninstantiable->module.get();
            auto tsm = dynamic_cast<wabt::TextScriptModule*>(m);
            RELEASE_ASSERT(tsm);
            auto buf = readModuleData(&tsm->module);
            auto trapResult = executeWASM(store, filename, buf->data, functionTypes, &registeredInstanceMap);
            std::string& s = trapResult.exception->message();
            RELEASE_ASSERT(s.find(assertModuleUninstantiable->text) == 0);
            printf("assertModuleUninstantiable (expect exception: %s(line: %d)) : OK\n", assertModuleUninstantiable->text.data(), assertModuleUninstantiable->module->location().line);
            break;
        }
        case wabt::CommandType::Register: {
            auto* registerCommand = static_cast<wabt::RegisterCommand*>(command.get());
            registeredInstanceMap[registerCommand->module_name] = fetchInstance(registerCommand->var, instanceMap, registeredInstanceMap);
            break;
        }
        case wabt::CommandType::Action: {
            auto* actionCommand = static_cast<wabt::ActionCommand*>(command.get());
            auto value = fetchInstance(actionCommand->action->module_var, instanceMap, registeredInstanceMap)->resolveExportFunction(actionCommand->action->name);
            RELEASE_ASSERT(value);
            if (actionCommand->action->type() == wabt::ActionType::Invoke) {
                auto action = static_cast<wabt::InvokeAction*>(actionCommand->action.get());
                auto fn = fetchInstance(action->module_var, instanceMap, registeredInstanceMap)->resolveExportFunction(action->name);
                executeInvokeAction(action, fn, wabt::ConstVector(), nullptr);
            } else {
                ASSERT_NOT_REACHED();
            }
            break;
        }
        case wabt::CommandType::AssertInvalid: {
            auto* assertModuleInvalid = static_cast<wabt::AssertModuleCommand<wabt::CommandType::AssertInvalid>*>(command.get());
            auto m = assertModuleInvalid->module.get();
            auto tsm = dynamic_cast<wabt::TextScriptModule*>(m);
            auto dsm = dynamic_cast<wabt::BinaryScriptModule*>(m);
            RELEASE_ASSERT(tsm || dsm);
            std::vector<uint8_t> buf;
            if (tsm) {
                buf = readModuleData(&tsm->module)->data;
            } else {
                buf = dsm->data;
            }
            auto trapResult = executeWASM(store, filename, buf, functionTypes);
            RELEASE_ASSERT(trapResult.exception);
            std::string& actual = trapResult.exception->message();
            printf("assertModuleInvalid (expect compile error: '%s', actual '%s'(line: %d)) : OK\n", assertModuleInvalid->text.data(), actual.data(), assertModuleInvalid->module->location().line);
            break;
        }
        case wabt::CommandType::AssertMalformed: {
            // we don't need to run invalid wat
            auto* assertMalformed = static_cast<wabt::AssertModuleCommand<wabt::CommandType::AssertMalformed>*>(command.get());
            break;
        }
        case wabt::CommandType::AssertUnlinkable: {
            auto* assertUnlinkable = static_cast<wabt::AssertUnlinkableCommand*>(command.get());
            auto m = assertUnlinkable->module.get();
            auto tsm = dynamic_cast<wabt::TextScriptModule*>(m);
            auto dsm = dynamic_cast<wabt::BinaryScriptModule*>(m);
            RELEASE_ASSERT(tsm || dsm);
            std::vector<uint8_t> buf;
            if (tsm) {
                buf = readModuleData(&tsm->module)->data;
            } else {
                buf = dsm->data;
            }
            auto trapResult = executeWASM(store, filename, buf, functionTypes);
            RELEASE_ASSERT(trapResult.exception);
            break;
        }
        case wabt::CommandType::AssertExhaustion: {
            auto* assertExhaustion = static_cast<wabt::AssertExhaustionCommand*>(command.get());
            auto value = fetchInstance(assertExhaustion->action->module_var, instanceMap, registeredInstanceMap)->resolveExportFunction(assertExhaustion->action->name);
            RELEASE_ASSERT(value);
            if (assertExhaustion->action->type() == wabt::ActionType::Invoke) {
                auto action = static_cast<wabt::InvokeAction*>(assertExhaustion->action.get());
                auto fn = fetchInstance(action->module_var, instanceMap, registeredInstanceMap)->resolveExportFunction(action->name);
                executeInvokeAction(action, fn, wabt::ConstVector(), assertExhaustion->text.data());
            } else {
                ASSERT_NOT_REACHED();
            }
            break;
        }
        default: {
            RELEASE_ASSERT_NOT_REACHED();
            break;
        }
        }

        commandCount++;
    }
}

static void runExports(Store* store, const std::string& filename, const std::vector<uint8_t>& src, std::string& entry)
{
    auto parseResult = WASMParser::parseBinary(store, filename, src.data(), src.size());

    if (!parseResult.second.empty()) {
        fprintf(stderr, "parse error: %s\n", parseResult.second.c_str());
        return;
    }

    auto module = parseResult.first;

    const auto& importTypes = module->imports();

    ExternVector importValues;

    if (importTypes.size() != 0) {
        fprintf(stderr, "error: module has imports, but imports are not supported\n");
        return;
    }

    struct RunData {
        Module* module;
        ExternVector& importValues;
        ExportType* exp;
    } data = { module.value(), importValues, nullptr };
    Walrus::Trap trap;

    for (auto&& exp : module->exports()) {
        if (exp->exportType() == ExportType::Function) {
            if (entry != exp->name() && !entry.empty()) {
                continue;
            }

            data.exp = exp;
            trap.run([](ExecutionState& state, void* d) {
                RunData* data = reinterpret_cast<RunData*>(d);
                Instance* instance = data->module->instantiate(state, data->importValues);
                auto fn = instance->function(data->exp->itemIndex());
                FunctionType* fnType = fn->asDefinedFunction()->moduleFunction()->functionType();

                if (fnType->param().size() != 0) {
                    printf("warning: function %s has params, but params are not supported\n", data->exp->name().c_str());
                    return;
                }

                Walrus::ValueVector result;
                result.resize(fnType->result().size());
                fn->call(state, 0, nullptr, result.data());

                for (auto&& r : result) {
                    switch (r.type()) {
                    case Value::I32:
                        printf("%d\n", r.asI32());
                        break;
                    case Value::I64:
                        printf("%ld\n", r.asI64());
                        break;
                    case Value::F32:
                        printf("%.7f\n", r.asF32());
                        break;
                    case Value::F64:
                        printf("%.15lf\n", r.asF64());
                        break;
                    default:
                        printf("(unknown)\n");
                    }
                }
            },
                     &data);
        }
    }
}

int main(int argc, char* argv[])
{
#ifndef NDEBUG
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);
#endif

#ifdef M_MMAP_THRESHOLD
    mallopt(M_MMAP_THRESHOLD, 2048);
#endif
#ifdef M_MMAP_MAX
    mallopt(M_MMAP_MAX, 1024 * 1024);
#endif

    Engine* engine = new Engine();
    Store* store = new Store(engine);

    SpecTestFunctionTypes functionTypes;
    bool runAllExports = false;
    std::string entry;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            if (strcmp(argv[i], "--run-all-exports") == 0) {
                runAllExports = true;
                continue;
            }
            if (strcmp(argv[i], "--entry") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "error: --entry requires an argument\n");
                    return 1;
                }

                entry = argv[++i];

                continue;
            }
        }

        std::string filePath = argv[i];
        FILE* fp = fopen(filePath.data(), "r");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            auto sz = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            std::vector<uint8_t> buf;
            buf.resize(sz);
            fread(buf.data(), sz, 1, fp);
            fclose(fp);

            if (endsWith(filePath, "wasm")) {
                if (runAllExports || !entry.empty()) {
                    runExports(store, filePath, buf, entry);
                } else {
                    auto trapResult = executeWASM(store, filePath, buf, functionTypes);
                    if (trapResult.exception) {
                        fprintf(stderr, "Uncaught Exception: %s\n", trapResult.exception->message().data());
                        return -1;
                    }
                }
            } else if (endsWith(filePath, "wat") || endsWith(filePath, "wast")) {
                executeWAST(store, filePath, buf, functionTypes);
            }
        } else {
            printf("Cannot open file %s\n", argv[i]);
            return -1;
        }
    }

    // finalize
    delete store;
    delete engine;

    return 0;
}
