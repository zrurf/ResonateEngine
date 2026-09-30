#ifndef RESONATE_PAL_POSIX_FILE_DESCRIPTOR_H
#define RESONATE_PAL_POSIX_FILE_DESCRIPTOR_H

#include <cstdint>

#include <resonate/pal/io.h>

namespace resonate::pal::posix
{

/* The io layer keeps a descriptor in the opaque handle the header exposes. */
inline void* handleFromDescriptor(int descriptor)
{
    return reinterpret_cast<void*>(static_cast<std::intptr_t>(descriptor));
}

inline int descriptorFromHandle(const ResonateFileHandle* file)
{
    return file != nullptr ? static_cast<int>(reinterpret_cast<std::intptr_t>(file->handle)) : -1;
}

} // namespace resonate::pal::posix

#endif /* RESONATE_PAL_POSIX_FILE_DESCRIPTOR_H */
