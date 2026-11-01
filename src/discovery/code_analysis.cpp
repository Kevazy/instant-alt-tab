#include "code_analysis.h"

#include <algorithm>
#include <iterator>

namespace discovery {

namespace {

constexpr size_t kMaxUnboundedWalk = 0x10000;  // bytes: a function without a table entry is a small leaf

const ZydisDecoder& Decoder() {
    static const ZydisDecoder decoder = [] {
        ZydisDecoder d;
        ZydisDecoderInit(&d, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
        return d;
    }();
    return decoder;
}

int RegisterIndex(ZydisRegister reg) {
    if (reg == ZYDIS_REGISTER_RIP) return kRip;
    ZydisRegister full = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, reg);
    if (full >= ZYDIS_REGISTER_RAX && full <= ZYDIS_REGISTER_R15) return full - ZYDIS_REGISTER_RAX;
    return kNoRegister;
}

bool IsGeneralRegister(int index) { return index >= 0 && index < kRegisters; }

std::optional<uint32_t> AbsoluteRva(const PeFile& file, uint32_t rva, const ZydisDecodedInstruction& instruction,
                                    const ZydisDecodedOperand& operand) {
    ZyanU64 address;
    if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&instruction, &operand, file.ImageBase() + rva, &address)) ||
        address < file.ImageBase()) {
        return std::nullopt;
    }
    return static_cast<uint32_t>(address - file.ImageBase());
}

// `base` plus `delta`.
std::optional<Value> Offset(Value base, int64_t delta) {
    if (base.kind != Value::Kind::Pointer && base.kind != Value::Kind::Constant) return std::nullopt;
    base.number += delta;
    return base;
}

// The qword at `base` plus `delta`.
std::optional<Value> Load(Value base, int64_t delta) {
    int64_t offset = base.number + delta;
    if (base.kind != Value::Kind::Pointer || base.loads == Value::kMaxLoads || offset != static_cast<int32_t>(offset)) {
        return std::nullopt;
    }
    base.offsets[base.loads++] = static_cast<int32_t>(offset);
    base.number = 0;
    return base;
}

// A constant written to a register of `bits` bits (32-bit writes clear the upper half).
Value Constant(int64_t value, uint16_t bits) {
    return Value::Constant(bits == 32 ? static_cast<int64_t>(static_cast<uint32_t>(value)) : value);
}

// The value a recognised instruction puts in its destination register, if it is one of those.
std::optional<Value> Result(const ZydisDecodedInstruction& instruction, const ZydisDecodedOperand* operands,
                            const Step& step) {
    if (instruction.operand_count_visible < 2 || operands[0].type != ZYDIS_OPERAND_TYPE_REGISTER) return std::nullopt;
    const ZydisDecodedOperand& source = operands[1];
    bool full = operands[0].size == 64;
    switch (instruction.mnemonic) {
        case ZYDIS_MNEMONIC_MOV:
            if (source.type == ZYDIS_OPERAND_TYPE_IMMEDIATE) return Constant(source.imm.value.s, operands[0].size);
            if (source.type == ZYDIS_OPERAND_TYPE_REGISTER) {
                int from = RegisterIndex(source.reg.value);
                if (!IsGeneralRegister(from)) return Value{};
                const Value& value = step.before[from];
                if (full) return value;
                return value.kind == Value::Kind::Constant ? Constant(value.number, operands[0].size) : Value{};
            }
            if (source.type == ZYDIS_OPERAND_TYPE_MEMORY && full && step.memory && step.memory->index == kNoRegister) {
                return Load(step.memory->baseValue, step.memory->displacement).value_or(Value{});
            }
            return Value{};
        case ZYDIS_MNEMONIC_LEA:
            if (!step.memory || step.memory->index != kNoRegister) return Value{};
            if (step.memory->base == kRip && step.memory->rva) {
                return Value{Value::Kind::GlobalAddress, 0, 0, {}, *step.memory->rva};
            }
            return Offset(step.memory->baseValue, step.memory->displacement).value_or(Value{});
        case ZYDIS_MNEMONIC_XOR: {
            bool zeroed = source.type == ZYDIS_OPERAND_TYPE_REGISTER && source.reg.value == operands[0].reg.value;
            return zeroed ? Value::Constant(0) : Value{};
        }
        case ZYDIS_MNEMONIC_SUB:
            if (source.type == ZYDIS_OPERAND_TYPE_REGISTER && source.reg.value == operands[0].reg.value) {
                return Value::Constant(0);
            }
            [[fallthrough]];
        case ZYDIS_MNEMONIC_ADD:
            if (source.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && full) {
                int to = RegisterIndex(operands[0].reg.value);
                int64_t delta = instruction.mnemonic == ZYDIS_MNEMONIC_ADD ? source.imm.value.s : -source.imm.value.s;
                if (IsGeneralRegister(to)) return Offset(step.before[to], delta).value_or(Value{});
            }
            return Value{};
        default: return std::nullopt;
    }
}

}  // namespace

const Value& Step::Argument(int n) const {
    static constexpr Register kArguments[] = {kRcx, kRdx, kR8, kR9};
    return before[kArguments[n]];
}

std::vector<Step> Walk(const PeFile& file, uint32_t start, std::optional<uint32_t> end, bool startsWithParameters,
                       size_t maxSteps) {
    std::array<Value, kRegisters> registers{};
    if (startsWithParameters) {
        registers[kRcx] = Value::Parameter(0);
        registers[kRdx] = Value::Parameter(1);
        registers[kR8] = Value::Parameter(2);
        registers[kR9] = Value::Parameter(3);
    }
    std::vector<Step> steps;
    uint32_t limit = end.value_or(start + static_cast<uint32_t>(kMaxUnboundedWalk));
    for (uint32_t rva = start; rva < limit && steps.size() < maxSteps;) {
        auto bytes = file.Read(rva, std::min<size_t>(ZYDIS_MAX_INSTRUCTION_LENGTH, limit - rva));
        ZydisDecodedInstruction instruction;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        if (bytes.empty() ||
            !ZYAN_SUCCESS(ZydisDecoderDecodeFull(&Decoder(), bytes.data(), bytes.size(), &instruction, operands))) {
            break;
        }
        Step step;
        step.rva = rva;
        step.length = instruction.length;
        step.mnemonic = instruction.mnemonic;
        step.call = instruction.meta.category == ZYDIS_CATEGORY_CALL;
        step.jump = instruction.meta.category == ZYDIS_CATEGORY_UNCOND_BR;
        step.conditionalJump = instruction.meta.category == ZYDIS_CATEGORY_COND_BR;
        step.ret = instruction.meta.category == ZYDIS_CATEGORY_RET;
        step.before = registers;
        for (int i = 0; i < instruction.operand_count_visible; ++i) {
            const ZydisDecodedOperand& operand = operands[i];
            if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY) {
                Memory memory;
                memory.base = RegisterIndex(operand.mem.base);
                memory.index = RegisterIndex(operand.mem.index);
                memory.displacement = operand.mem.disp.value;
                memory.bits = operand.size;
                memory.written = (operand.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) != 0;
                if (memory.base == kRip) memory.rva = AbsoluteRva(file, rva, instruction, operand);
                if (IsGeneralRegister(memory.base)) memory.baseValue = registers[memory.base];
                step.memory = memory;
            } else if (operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                if (operand.imm.is_relative) {
                    step.target = AbsoluteRva(file, rva, instruction, operand);
                } else {
                    step.immediate = operand.imm.value.s;
                }
            }
        }

        // Every register the instruction writes becomes unknown, unless it is
        // one of the few instructions whose result is tracked.
        std::optional<Value> result = Result(instruction, operands, step);
        for (int i = 0; i < instruction.operand_count; ++i) {
            const ZydisDecodedOperand& operand = operands[i];
            if (operand.type == ZYDIS_OPERAND_TYPE_REGISTER && (operand.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE)) {
                if (int index = RegisterIndex(operand.reg.value); IsGeneralRegister(index)) registers[index] = Value{};
            }
        }
        if (result) registers[RegisterIndex(operands[0].reg.value)] = *result;
        if (step.call) {
            for (Register clobbered : {kRax, kRcx, kRdx, kR8, kR9, kR10, kR11}) registers[clobbered] = Value{};
        }
        steps.push_back(step);
        rva += instruction.length;
        if (!end && (step.ret || step.jump)) break;
    }
    return steps;
}

std::vector<Step> WalkFunction(const PeFile& file, uint32_t function) {
    return Walk(file, function, file.FunctionEnd(function));
}

CodeIndex::CodeIndex(const PeFile& file) {
    for (const RUNTIME_FUNCTION& part : file.Functions()) {
        auto function = file.FunctionContaining(part.BeginAddress);
        if (!function) continue;
        for (const Step& step : Walk(file, part.BeginAddress, part.EndAddress, *function == part.BeginAddress)) {
            if (step.call && step.target) {
                calls_.push_back({*function, step.rva, *step.target, step.Argument(0), step.Argument(1)});
            }
            if (step.memory && step.memory->rva) references_.push_back({*function, *step.memory->rva});
        }
    }
}

std::vector<CallSite> CodeIndex::CallsTo(uint32_t target) const {
    std::vector<CallSite> found;
    std::copy_if(calls_.begin(), calls_.end(), std::back_inserter(found),
                 [&](const CallSite& call) { return call.target == target; });
    return found;
}

std::vector<CallSite> CodeIndex::CallsFrom(uint32_t function) const {
    std::vector<CallSite> found;
    std::copy_if(calls_.begin(), calls_.end(), std::back_inserter(found),
                 [&](const CallSite& call) { return call.function == function; });
    return found;
}

std::set<uint32_t> CodeIndex::CallTargets() const {
    std::set<uint32_t> targets;
    for (const CallSite& call : calls_) targets.insert(call.target);
    return targets;
}

std::vector<uint32_t> CodeIndex::FunctionsReferencing(uint32_t rva) const {
    std::vector<uint32_t> found;
    for (const Reference& reference : references_) {
        if (reference.rva == rva && std::find(found.begin(), found.end(), reference.function) == found.end()) {
            found.push_back(reference.function);
        }
    }
    return found;
}

std::optional<uint32_t> VtableSlotCalled(const std::vector<Step>& steps, int depth) {
    std::optional<uint32_t> slot;
    for (const Step& step : steps) {
        if (!step.call) continue;
        // The method is either called through a vtable entry, or loaded into rax for the CFG dispatch
        // thunk, which is called directly or through a global pointer.
        bool throughRegister = step.memory && step.memory->base != kRip;
        std::optional<Value> method = throughRegister ? Load(step.memory->baseValue, step.memory->displacement)
                                                      : std::optional(step.before[kRax]);
        if (!method || method->kind != Value::Kind::Pointer || method->parameter != 0 || method->loads != depth + 1 ||
            method->number != 0) {
            continue;
        }
        bool throughVtable = std::all_of(method->offsets.begin(), method->offsets.begin() + depth,
                                         [](int32_t offset) { return offset == 0; });
        int32_t offset = method->offsets[depth];
        if (!throughVtable || offset < 0 || offset % 8 != 0) continue;
        if (slot && *slot != static_cast<uint32_t>(offset / 8)) return std::nullopt;  // more than one slot
        slot = offset / 8;
    }
    return slot;
}

}  // namespace discovery
