#include "reflection.h"

#include <unordered_map>
#include <vector>

namespace resonate::render_vulkan {
namespace {

constexpr std::uint32_t kSpirvMagic = 0x07230203U;
constexpr std::uint32_t kMaxRecursion = 32U;

enum Opcode : std::uint32_t
{
    kOpName = 5,
    kOpEntryPoint = 15,
    kOpTypeInt = 21,
    kOpTypeFloat = 22,
    kOpTypeVector = 23,
    kOpTypeMatrix = 24,
    kOpTypeImage = 25,
    kOpTypeSampler = 26,
    kOpTypeSampledImage = 27,
    kOpTypeArray = 28,
    kOpTypeStruct = 30,
    kOpTypeRuntimeArray = 31,
    kOpTypePointer = 32,
    kOpConstant = 43,
    kOpDecorate = 71,
    kOpMemberDecorate = 72,
    kOpVariable = 59,
};

enum Decoration : std::uint32_t
{
    kDecorationBinding = 33,
    kDecorationDescriptorSet = 34,
    kDecorationOffset = 35,
};

enum StorageClass : std::uint32_t
{
    kStorageUniformConstant = 0,
    kStorageUniform = 2,
    kStorageStorageBuffer = 12,
    kStoragePushConstant = 9,
};

enum ExecutionModel : std::uint32_t
{
    kModelVertex = 0,
    kModelFragment = 4,
    kModelGlCompute = 5,
};

struct Type
{
    enum Kind
    {
        Other,
        Scalar,
        Vector,
        Matrix,
        Array,
        RuntimeArray,
        Struct,
        Image,
        Sampler,
        SampledImage,
        Pointer,
    } kind = Other;
    std::uint32_t width = 0;          /* scalars: bits */
    std::uint32_t count = 0;          /* vectors: components; matrices: columns */
    std::uint32_t element = 0;        /* arrays and members: element type */
    std::uint64_t length = 0;         /* arrays: element count */
    std::uint32_t storage = 0;        /* pointers: storage class */
    std::uint32_t target = 0;         /* pointers: pointee type */
    std::vector<std::uint32_t> members;
};

/* Key for a struct member's decorated byte offset: (type id, member index). */
std::uint64_t memberKey(std::uint32_t type, std::uint32_t member)
{
    return (static_cast<std::uint64_t>(type) << 16U) | member;
}

struct Module
{
    std::unordered_map<std::uint32_t, Type> types;
    std::unordered_map<std::uint32_t, std::uint64_t> constants;
    std::unordered_map<std::uint32_t, std::string> names;
    std::unordered_map<std::uint32_t, std::uint32_t> bindings;
    std::unordered_map<std::uint32_t, std::uint32_t> sets;
    std::unordered_map<std::uint64_t, std::uint32_t> member_offsets;
    bool stage_found = false;
};

/* Byte size of a type, resolved through vectors, matrices, arrays and structs.
   Answers false for anything else — a push block must be plain data. */
bool typeSize(const Module& module, std::uint32_t type, std::uint32_t depth,
              std::uint64_t& out_size)
{
    if (depth > kMaxRecursion)
    {
        return false;
    }
    const auto found = module.types.find(type);
    if (found == module.types.end())
    {
        return false;
    }
    const Type& t = found->second;
    switch (t.kind)
    {
        case Type::Pointer:
            return typeSize(module, t.target, depth + 1U, out_size);
        case Type::Scalar:
            out_size = t.width / 8U;
            return true;
        case Type::Vector:
        {
            std::uint64_t component = 0U;
            if (!typeSize(module, t.element, depth + 1U, component))
            {
                return false;
            }
            out_size = component * t.count;
            return true;
        }
        case Type::Matrix:
        {
            std::uint64_t column = 0U;
            if (!typeSize(module, t.element, depth + 1U, column))
            {
                return false;
            }
            out_size = column * t.count;
            return true;
        }
        case Type::Array:
        {
            std::uint64_t element = 0U;
            if (!typeSize(module, t.element, depth + 1U, element))
            {
                return false;
            }
            out_size = element * t.length;
            return true;
        }
        case Type::Struct:
        {
            std::uint64_t total = 0U;
            for (std::uint32_t member = 0U; member < t.members.size(); ++member)
            {
                const auto offset = module.member_offsets.find(memberKey(type, member));
                if (offset == module.member_offsets.end())
                {
                    return false;
                }
                std::uint64_t size = 0U;
                if (!typeSize(module, t.members[member], depth + 1U, size))
                {
                    return false;
                }
                total = total > offset->second + size ? total : offset->second + size;
            }
            out_size = total;
            return true;
        }
        default:
            return false;
    }
}

/* Follows pointers and arrays from a variable's type down to the resource
   itself, counting array elements on the way. */
bool resolveResource(const Module& module, std::uint32_t type, std::uint64_t& out_elements,
                     Type::Kind& out_kind, std::uint32_t& out_storage)
{
    std::uint64_t elements = 1U;
    for (std::uint32_t depth = 0U; depth <= kMaxRecursion; ++depth)
    {
        const auto found = module.types.find(type);
        if (found == module.types.end())
        {
            return false;
        }
        const Type& t = found->second;
        if (t.kind == Type::Pointer)
        {
            out_storage = t.storage;
            type = t.target;
            continue;
        }
        if (t.kind == Type::Array)
        {
            elements *= t.length;
            type = t.element;
            continue;
        }
        out_elements = elements;
        out_kind = t.kind;
        return true;
    }
    return false;
}

std::string nameOf(const Module& module, std::uint32_t id)
{
    const auto found = module.names.find(id);
    return found != module.names.end() ? found->second : std::string{};
}

} // namespace

bool reflectShader(const std::uint32_t* words, std::uint64_t word_count, ShaderStageKind stage,
                   const ShaderContract& contract, std::string& error)
{
    error.clear();
    if (words == nullptr || word_count < 5U || words[0] != kSpirvMagic)
    {
        error = "shader is not a SPIR-V module";
        return false;
    }

    Module module;
    std::uint64_t index = 5U;
    while (index < word_count)
    {
        const std::uint32_t word = words[index];
        const std::uint32_t count = word >> 16U;
        const std::uint32_t opcode = word & 0xFFFFU;
        if (count == 0U || index + count > word_count)
        {
            error = "shader SPIR-V is truncated";
            return false;
        }
        const std::uint32_t* operands = words + index + 1U;
        const std::uint32_t operand_count = count - 1U;

        switch (opcode)
        {
            case kOpName:
            {
                /* [target, NUL-terminated name literals]. */
                if (operand_count >= 2U)
                {
                    module.names[operands[0]] =
                        std::string(reinterpret_cast<const char*>(operands + 1U));
                }
                break;
            }
            case kOpEntryPoint:
            {
                if (operand_count >= 1U)
                {
                    const std::uint32_t model = operands[0];
                    module.stage_found =
                        module.stage_found ||
                        (stage == ShaderStageKind::Vertex && model == kModelVertex) ||
                        (stage == ShaderStageKind::Fragment && model == kModelFragment) ||
                        (stage == ShaderStageKind::Compute && model == kModelGlCompute);
                }
                break;
            }
            case kOpTypeInt:
            {
                if (operand_count >= 3U)
                {
                    Type& t = module.types[operands[0]];
                    t.kind = Type::Scalar;
                    t.width = operands[1];
                }
                break;
            }
            case kOpTypeFloat:
            {
                if (operand_count >= 2U)
                {
                    Type& t = module.types[operands[0]];
                    t.kind = Type::Scalar;
                    t.width = operands[1];
                }
                break;
            }
            case kOpTypeVector:
            {
                if (operand_count >= 3U)
                {
                    Type& t = module.types[operands[0]];
                    t.kind = Type::Vector;
                    t.element = operands[1];
                    t.count = operands[2];
                }
                break;
            }
            case kOpTypeMatrix:
            {
                if (operand_count >= 3U)
                {
                    Type& t = module.types[operands[0]];
                    t.kind = Type::Matrix;
                    t.element = operands[1];
                    t.count = operands[2];
                }
                break;
            }
            case kOpTypeImage:
            {
                if (operand_count >= 2U)
                {
                    module.types[operands[0]].kind = Type::Image;
                }
                break;
            }
            case kOpTypeSampler:
            {
                if (operand_count >= 1U)
                {
                    module.types[operands[0]].kind = Type::Sampler;
                }
                break;
            }
            case kOpTypeSampledImage:
            {
                if (operand_count >= 2U)
                {
                    Type& t = module.types[operands[0]];
                    t.kind = Type::SampledImage;
                    t.element = operands[1];
                }
                break;
            }
            case kOpTypeArray:
            {
                if (operand_count >= 3U)
                {
                    Type& t = module.types[operands[0]];
                    t.kind = Type::Array;
                    t.element = operands[1];
                    const auto length = module.constants.find(operands[2]);
                    t.length = length != module.constants.end() ? length->second : 0U;
                }
                break;
            }
            case kOpTypeRuntimeArray:
            {
                if (operand_count >= 2U)
                {
                    Type& t = module.types[operands[0]];
                    t.kind = Type::RuntimeArray;
                    t.element = operands[1];
                }
                break;
            }
            case kOpTypeStruct:
            {
                if (operand_count >= 1U)
                {
                    Type& t = module.types[operands[0]];
                    t.kind = Type::Struct;
                    t.members.assign(operands + 1U, operands + operand_count);
                }
                break;
            }
            case kOpTypePointer:
            {
                if (operand_count >= 3U)
                {
                    Type& t = module.types[operands[0]];
                    t.kind = Type::Pointer;
                    t.storage = operands[1];
                    t.target = operands[2];
                }
                break;
            }
            case kOpConstant:
            {
                /* [type, result, value words]: the tables' lengths are 32-bit. */
                if (operand_count >= 3U)
                {
                    module.constants[operands[1]] = operands[2];
                }
                break;
            }
            case kOpDecorate:
            {
                if (operand_count >= 3U)
                {
                    if (operands[1] == kDecorationBinding)
                    {
                        module.bindings[operands[0]] = operands[2];
                    }
                    else if (operands[1] == kDecorationDescriptorSet)
                    {
                        module.sets[operands[0]] = operands[2];
                    }
                }
                break;
            }
            case kOpMemberDecorate:
            {
                if (operand_count >= 4U && operands[2] == kDecorationOffset)
                {
                    module.member_offsets[memberKey(operands[0], operands[1])] = operands[3];
                }
                break;
            }
            default:
                break;
        }
        index += count;
    }

    if (!module.stage_found)
    {
        error = "shader declares no entry point of the requested stage";
        return false;
    }

    /* Second pass over the variables themselves: each carries its result type
       (the pointer), its storage class and the decorations the first pass kept. */
    index = 5U;
    while (index < word_count)
    {
        const std::uint32_t word = words[index];
        const std::uint32_t count = word >> 16U;
        const std::uint32_t opcode = word & 0xFFFFU;
        if (count == 0U || index + count > word_count)
        {
            error = "shader SPIR-V is truncated";
            return false;
        }
        if (opcode == kOpVariable && count >= 4U)
        {
            const std::uint32_t type_id = words[index + 1U];
            const std::uint32_t id = words[index + 2U];
            const std::uint32_t storage = words[index + 3U];

            if (storage == kStoragePushConstant)
            {
                std::uint64_t size = 0U;
                if (!typeSize(module, type_id, 0U, size))
                {
                    error = "shader push constant block is not plain data";
                    return false;
                }
                if (size > contract.max_push_constants)
                {
                    error = "shader push constants use " + std::to_string(size) +
                            " bytes; the limit is " + std::to_string(contract.max_push_constants);
                    return false;
                }
                index += count;
                continue;
            }
            if (storage != kStorageUniformConstant && storage != kStorageUniform &&
                storage != kStorageStorageBuffer)
            {
                index += count;
                continue;
            }

            const auto set = module.sets.find(id);
            if (set == module.sets.end())
            {
                error = "shader resource '" + nameOf(module, id) + "' declares no descriptor set";
                return false;
            }
            if (set->second != 0U)
            {
                error = "shader resource '" + nameOf(module, id) + "' declares set " +
                        std::to_string(set->second) + "; the engine binds set 0 only";
                return false;
            }
            const auto binding = module.bindings.find(id);
            if (binding == module.bindings.end())
            {
                error = "shader resource '" + nameOf(module, id) + "' declares no binding";
                return false;
            }
            const std::uint32_t slot = binding->second;
            std::uint64_t elements = 0U;
            Type::Kind kind = Type::Other;
            std::uint32_t element_storage = 0U;
            if (!resolveResource(module, type_id, elements, kind, element_storage))
            {
                error = "shader resource '" + nameOf(module, id) + "' has an unreadable type";
                return false;
            }

            const std::string name = nameOf(module, id);
            const std::string quoted = "'" + (name.empty() ? std::to_string(id) : name) + "' ";
            const std::string declared = " declares " + std::to_string(elements) + " entries";
            if (slot >= contract.storage_bindings_begin && slot < contract.storage_bindings_end)
            {
                /* slangc lowers StructuredBuffer arrays to Uniform-class blocks;
                   a StorageBuffer-class pointee is the same contract. */
                if (kind != Type::Struct || elements != contract.storage_table_size)
                {
                    error = "shader storage table binding " + std::to_string(slot) + quoted +
                            declared + "; the table holds " +
                            std::to_string(contract.storage_table_size);
                    return false;
                }
            }
            else if (slot == contract.sampled_table_binding)
            {
                /* R13 split the table: it carries images, and the sampler comes
                   from the palette at the sampling site. */
                if (kind != Type::Image || elements != contract.sampled_table_size)
                {
                    error = "shader image table binding " + std::to_string(slot) + quoted +
                            declared + "; the table holds " +
                            std::to_string(contract.sampled_table_size);
                    return false;
                }
            }
            else if (slot == contract.sampler_table_binding)
            {
                if (kind != Type::Sampler || elements != contract.sampler_table_size)
                {
                    error = "shader sampler table binding " + std::to_string(slot) + quoted +
                            declared + "; the palette holds " +
                            std::to_string(contract.sampler_table_size);
                    return false;
                }
            }
            else if (slot == contract.frame_binding)
            {
                if (element_storage != kStorageUniform || elements != 1U)
                {
                    error = "shader frame block binding " + std::to_string(slot) + quoted +
                            declared + "; the engine binds one block";
                    return false;
                }
            }
            else if (slot == contract.shadow_table_binding)
            {
                if (kind != Type::SampledImage || elements != contract.shadow_table_size)
                {
                    error = "shader shadow table binding " + std::to_string(slot) + quoted +
                            declared + "; the table holds " +
                            std::to_string(contract.shadow_table_size);
                    return false;
                }
            }
            else
            {
                error = "shader binding " + std::to_string(slot) +
                        " is outside the engine's tables";
                return false;
            }
        }
        index += count;
    }
    return true;
}

} // namespace resonate::render_vulkan
