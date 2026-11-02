#include <resonate/application/save.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

#include <resonate/core/blake3.h>

namespace resonate::save
{
namespace
{

constexpr char kMagic[4] = {'R', 'S', 'A', 'V'};
constexpr std::size_t kHeaderBytes = 4U + 4U + 8U;
constexpr std::size_t kDigestBytes = 32U;

void appendU32(std::vector<std::byte>& out, std::uint32_t value)
{
    for (int byte = 0; byte < 4; ++byte)
    {
        out.push_back(std::byte{static_cast<std::uint8_t>((value >> (8 * byte)) & 0xFFU)});
    }
}

void appendU64(std::vector<std::byte>& out, std::uint64_t value)
{
    for (int byte = 0; byte < 8; ++byte)
    {
        out.push_back(std::byte{static_cast<std::uint8_t>((value >> (8 * byte)) & 0xFFU)});
    }
}

std::uint32_t readU32(Span<const std::byte> bytes, std::size_t offset)
{
    std::uint32_t value = 0U;
    for (std::size_t byte = 0U; byte < 4U; ++byte)
    {
        const std::uint32_t part = std::to_integer<std::uint32_t>(bytes[offset + byte]);
        value |= part << (8U * static_cast<unsigned>(byte));
    }
    return value;
}

std::uint64_t readU64(Span<const std::byte> bytes, std::size_t offset)
{
    std::uint64_t value = 0U;
    for (std::size_t byte = 0U; byte < 8U; ++byte)
    {
        const std::uint64_t part = std::to_integer<std::uint64_t>(bytes[offset + byte]);
        value |= part << (8U * static_cast<unsigned>(byte));
    }
    return value;
}

bool validName(const std::string& name)
{
    if (name.empty() || name.size() > 64U || name.front() == '.')
    {
        return false;
    }
    for (const char character : name)
    {
        const bool plain =
            (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '.' || character == '_' ||
            character == '-';
        if (!plain)
        {
            return false;
        }
    }
    return name.find("..") == std::string::npos;
}

} // namespace

std::vector<std::byte> encodeSave(const SaveFile& file)
{
    std::vector<std::byte> bytes;
    bytes.reserve(kHeaderBytes + file.payload.size() + kDigestBytes);
    for (const char character : kMagic)
    {
        bytes.push_back(std::byte{static_cast<std::uint8_t>(character)});
    }
    appendU32(bytes, file.version);
    appendU64(bytes, file.payload.size());
    bytes.insert(bytes.end(), file.payload.begin(), file.payload.end());

    const hash::Digest digest = hash::blake3(bytes.data(), bytes.size());
    for (const std::uint8_t byte : digest)
    {
        bytes.push_back(std::byte{byte});
    }
    return bytes;
}

bool decodeSave(Span<const std::byte> bytes, SaveFile& out, std::string& error)
{
    if (bytes.size() < kHeaderBytes + kDigestBytes)
    {
        error = "the save is truncated";
        return false;
    }
    for (std::size_t index = 0; index < sizeof(kMagic); ++index)
    {
        if (std::to_integer<std::uint8_t>(bytes[index]) !=
            static_cast<std::uint8_t>(kMagic[index]))
        {
            error = "the file is not a save";
            return false;
        }
    }
    const std::uint32_t version = readU32(bytes, 4U);
    if (version == 0U)
    {
        error = "the save names version zero";
        return false;
    }
    const std::uint64_t payload_size = readU64(bytes, 8U);
    if (payload_size != bytes.size() - kHeaderBytes - kDigestBytes)
    {
        error = "the save's payload length does not match its file";
        return false;
    }

    const hash::Digest actual =
        hash::blake3(bytes.data(), bytes.size() - kDigestBytes);
    hash::Digest recorded{};
    std::memcpy(recorded.data(), bytes.data() + bytes.size() - kDigestBytes, kDigestBytes);
    if (!(actual == recorded))
    {
        error = "the save is corrupt";
        return false;
    }

    out.version = version;
    out.payload.assign(bytes.data() + kHeaderBytes, bytes.data() + kHeaderBytes + payload_size);
    return true;
}

SaveStore::SaveStore(std::string root) : root_(std::move(root))
{
}

std::optional<std::string> SaveStore::pathOf(const std::string& name) const
{
    if (!validName(name))
    {
        return std::nullopt;
    }
    return (std::filesystem::path(root_) / "saves" / (name + ".sav")).string();
}

bool SaveStore::write(const std::string& name, const SaveFile& file, std::string& error)
{
    const std::optional<std::string> target = pathOf(name);
    if (!target)
    {
        error = "the save name '" + name + "' is not a plain file stem";
        return false;
    }
    std::error_code code;
    std::filesystem::create_directories(std::filesystem::path(*target).parent_path(), code);
    if (code)
    {
        error = "cannot create the save directory for " + *target;
        return false;
    }
    const std::vector<std::byte> bytes = encodeSave(file);
    const std::string temporary = *target + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
        {
            error = "cannot write " + *target;
            return false;
        }
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        if (!out.good())
        {
            std::filesystem::remove(temporary, code);
            error = "cannot write " + *target;
            return false;
        }
    }
    std::filesystem::rename(temporary, *target, code);
    if (code)
    {
        /* Some platforms refuse to rename over an existing file; replacing it
           is the documented outcome, and the temporary is still intact. */
        std::error_code ignored;
        std::filesystem::remove(*target, ignored);
        code.clear();
        std::filesystem::rename(temporary, *target, code);
    }
    if (code)
    {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        error = "cannot replace " + *target;
        return false;
    }
    return true;
}

bool SaveStore::read(const std::string& name, SaveFile& out, std::string& error) const
{
    const std::optional<std::string> path = pathOf(name);
    if (!path)
    {
        error = "the save name '" + name + "' is not a plain file stem";
        return false;
    }
    std::ifstream in(*path, std::ios::binary);
    if (!in.is_open())
    {
        error = "cannot open " + *path;
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff length = in.tellg();
    if (length < 0)
    {
        error = "cannot read " + *path;
        return false;
    }
    in.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(static_cast<std::size_t>(length));
    if (!bytes.empty() &&
        !in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
    {
        error = "cannot read " + *path;
        return false;
    }
    if (!decodeSave(Span<const std::byte>(bytes.data(), bytes.size()), out, error))
    {
        error = *path + ": " + error;
        return false;
    }
    return true;
}

bool SaveStore::remove(const std::string& name, std::string& error)
{
    const std::optional<std::string> path = pathOf(name);
    if (!path)
    {
        error = "the save name '" + name + "' is not a plain file stem";
        return false;
    }
    std::error_code code;
    std::filesystem::remove(*path, code);
    if (code)
    {
        error = "cannot remove " + *path;
        return false;
    }
    return true;
}

std::string defaultUserDataRoot(const std::string& application)
{
    if (const char* staged = std::getenv("RESONATE_USER_DATA");
        staged != nullptr && *staged != '\0')
    {
        return staged;
    }
#if defined(_WIN32)
    if (const char* local = std::getenv("LOCALAPPDATA"); local != nullptr && *local != '\0')
    {
        return (std::filesystem::path(local) / application).string();
    }
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0')
    {
        return (std::filesystem::path(home) / "Library" / "Application Support" / application)
            .string();
    }
#else
    if (const char* data = std::getenv("XDG_DATA_HOME"); data != nullptr && *data != '\0')
    {
        return (std::filesystem::path(data) / application).string();
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0')
    {
        return (std::filesystem::path(home) / ".local" / "share" / application).string();
    }
#endif
    return {};
}

} // namespace resonate::save
