#include <resonate/dom/abi.h>

/* Compiles the C surface as C, not only as C++. Called from the C++ ABI test
   so the boundary is linked as well as parsed. */
int resonate_dom_c_abi_check(void)
{
    ResonateDomDocument* document = resonate_dom_create();
    if (document == 0)
    {
        return 1;
    }
    const ResonateDomString type = resonate_dom_intern(document, "Group", 5);
    const ResonateDomNode node = resonate_dom_create_node(document, type);
    const int ok = node.value != 0 && resonate_dom_alive(document, node) == 1;
    resonate_dom_destroy(document);
    return ok ? 0 : 1;
}
