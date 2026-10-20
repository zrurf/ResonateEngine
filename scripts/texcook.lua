-- Cooks the source textures under content/render/textures into the runtime
-- container. v1 writes uncompressed 2D RGBA8 KTX2 (VK_FORMAT_R8G8B8A8_UNORM):
-- the container and the loader are the contract, and the compressed
-- Basis/BC path replaces the writer when the asset pipeline lands.
--
-- Run with `xmake texcook`, or through the renderer's before_build hook.

local function readAll(file_path)
    local file = io.open(file_path, "rb")
    if not file then
        return nil
    end
    local data = file:read("*a")
    file:close()
    return data
end

local function writeAll(file_path, data)
    local file = assert(io.open(file_path, "wb"))
    file:write(data)
    file:close()
end

-- P6 PPM: an ASCII header (whitespace and # comments between tokens), then
-- width * height * 3 bytes after a single separator.
local function readPpm(file_path)
    local data = readAll(file_path)
    if data == nil then
        return nil, "cannot read"
    end
    local position = 1
    local function token()
        while position <= #data do
            local c = data:sub(position, position)
            if c == "#" then
                while position <= #data and data:sub(position, position) ~= "\n" do
                    position = position + 1
                end
            elseif c:match("%s") then
                position = position + 1
            else
                break
            end
        end
        local start = position
        while position <= #data and not data:sub(position, position):match("%s") do
            position = position + 1
        end
        return data:sub(start, position - 1)
    end

    local magic = token()
    local width = tonumber(token())
    local height = tonumber(token())
    local maxval = tonumber(token())
    if magic ~= "P6" or width == nil or height == nil or maxval ~= 255 then
        return nil, "not a P6 PPM with maxval 255"
    end
    position = position + 1 -- the separator after maxval
    local rgb = data:sub(position, position + width * height * 3 - 1)
    if #rgb ~= width * height * 3 then
        return nil, "short pixel data"
    end
    return {width = width, height = height, rgb = rgb}
end

local function u16(value)
    return string.pack("<I2", value)
end

local function u32(value)
    return string.pack("<I4", value)
end

local function u64(value)
    return string.pack("<I8", value)
end

-- The basic DFD for 8-bit RGBA UNORM: four 16-byte sample descriptors, one
-- channel each at bit offsets 0/8/16/24.
local function dfd()
    local samples = {}
    for channel = 0, 3 do
        local descriptor = u16(channel * 8)
            .. string.pack("BB", 8, 0)                 -- bit length, channel type UNORM
            .. string.pack("BBBB", channel, 0, 0, 0)   -- sample position in the texel
            .. u32(0) .. u32(255)
        samples[#samples + 1] = descriptor
    end
    local block = u16(0) .. u16(0) .. u16(2) .. u16(88)   -- vendor, type, version, block size
        .. string.pack("BBBB", 0, 0, 2, 0)                 -- RGBSDA, primaries, linear, flags
        .. string.pack("BBBB", 0, 0, 0, 0)                 -- texel block dimensions minus one
        .. string.pack("BBBBBBBB", 4, 0, 0, 0, 0, 0, 0, 0) -- bytes per plane
        .. table.concat(samples)
    assert(#block == 88, "unexpected DFD block size")
    return u32(4 + #block) .. block
end

-- KTX2: header (80) + level index (24) + DFD + pad + level 0.
local function ktx2(width, height, rgba)
    local identifier = string.char(0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB,
                                   0x0D, 0x0A, 0x1A, 0x0A)
    local data = dfd()
    local dfd_offset = 80 + 24
    local kvd_offset = dfd_offset + #data
    local level_offset = (kvd_offset + 7) & ~7
    local level_length = width * height * 4
    local header = identifier
        .. u32(37)                                  -- VK_FORMAT_R8G8B8A8_UNORM
        .. u32(1)                                   -- typeSize
        .. u32(width) .. u32(height) .. u32(0)      -- extent, depth 0 for 2D
        .. u32(0) .. u32(1) .. u32(1) .. u32(0)     -- layers, faces, levels, supercompression
        .. u32(dfd_offset) .. u32(#data)
        .. u32(kvd_offset) .. u32(0)
        .. u64(0) .. u64(0)
        .. u64(level_offset) .. u64(level_length) .. u64(level_length)
    assert(#header == 104, "unexpected KTX2 header size")
    local padding = string.rep("\0", level_offset - (#header + #data))
    return header .. data .. padding .. rgba
end

local function main()
    local project = os.projectdir()
    local source_dir = path.join(project, "content", "render", "textures")
    local output_dir = path.join(project, "build", "content", "textures")
    os.mkdir(output_dir)

    local written = 0
    for _, source in ipairs(os.files(path.join(source_dir, "*.ppm"))) do
        local name = path.basename(source):gsub("%.ppm$", "")
        local image, error_message = readPpm(source)
        if image == nil then
            raise("texcook: %s: %s", source, error_message)
        end
        local blocks = {}
        for pixel = 1, image.width * image.height do
            local r, g, b = image.rgb:byte((pixel - 1) * 3 + 1, (pixel - 1) * 3 + 3)
            blocks[#blocks + 1] = string.char(r, g, b, 255)
        end
        writeAll(path.join(output_dir, name .. ".ktx2"),
                 ktx2(image.width, image.height, table.concat(blocks)))
        written = written + 1
    end
    print(string.format("texcook: %d texture(s) -> %s", written,
                        path.relative(output_dir, project)))
end

main()
