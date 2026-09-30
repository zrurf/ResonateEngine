#include <resonate/module/abi.h>

#include <resonate/module/capability_id.hpp>

/* The C entry point onto the one hash implementation, so an id read from a
   manifest at run time and one a capability header declares cannot disagree. */
ResonateId resonate_id_make(const char* name)
{
    return resonate::Id(name != nullptr ? name : "").value();
}
