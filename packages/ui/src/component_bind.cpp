#include "component_bind.h"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <vector>

#include <resonate/core/math.h>

#include "component_bind.h"

#include <resonate/document_bind/bind.h>

#include <string>

namespace resonate::ui::detail
{

bool parseField(const dom::ComponentFieldDesc& field, const std::string& text, std::byte* data,
                std::string& error)
{
    return resonate::dom_bind::parseField(field, text, data, error);
}

bool attachGeneric(ecs::World& world, const dom::Document& document, dom::NodeHandle node,
                   ecs::Entity entity, const dom::Component& component,
                   const dom::ComponentDesc& descriptor, std::string& error)
{
    (void)node;
    return resonate::dom_bind::attachComponent(world, entity, document, component, descriptor,
                                               error);
}

} // namespace resonate::ui::detail
