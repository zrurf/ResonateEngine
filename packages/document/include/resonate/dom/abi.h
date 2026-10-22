#ifndef RESONATE_DOM_ABI_H
#define RESONATE_DOM_ABI_H

/*
 * The document tree's C surface: the language boundary the editor, the cook
 * tool and generated bindings share. Node handles are values, strings live in
 * the document's intern table, and every pointer returned here is borrowed
 * until the next mutation of the document (or, for strings, until the document
 * is destroyed).
 *
 * The text parser here resolves the explicit grammar only; implicit node types
 * need a schema registry and go through the C++ `Document::parse` overload.
 */

#include <stdint.h>

#include <resonate/module/abi.h>

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct ResonateDomDocument ResonateDomDocument;

/* A node is a slot plus generation; {0} is null and an old generation reads as
   invalid. */
typedef struct ResonateDomNode
{
    uint64_t value;
} ResonateDomNode;

#define RESONATE_DOM_NODE_NULL {0}

/* An interned string. RESONATE_DOM_STRING_NULL is the absent value. */
typedef struct ResonateDomString
{
    uint32_t id;
} ResonateDomString;

#define RESONATE_DOM_STRING_NULL {0xFFFFFFFFu}

typedef struct ResonateDomDiagnostic
{
    uint32_t struct_size;
    uint32_t severity; /* 0 warning, 1 error */
    uint32_t line;
    uint32_t column;
    const char* message; /* valid for the duration of the callback */
} ResonateDomDiagnostic;

typedef void (*ResonateDomDiagnosticFn)(void* user, const ResonateDomDiagnostic* diagnostic);

/* --- document --- */

ResonateDomDocument* resonate_dom_create(void);
void resonate_dom_destroy(ResonateDomDocument* document);

/* Replaces the document on success; a failed parse leaves it unchanged. */
ResonateStatus resonate_dom_parse(ResonateDomDocument* document, const char* utf8,
                                  uint32_t size_bytes, ResonateDomDiagnosticFn diagnostics,
                                  void* user);

/* Canonical explicit XML. Call with out_utf8 null (or a small capacity) first
   to learn the size; the return value is always the full byte count. */
uint32_t resonate_dom_xml_size(const ResonateDomDocument* document);
uint32_t resonate_dom_xml(const ResonateDomDocument* document, char* out_utf8,
                          uint32_t capacity);

uint64_t resonate_dom_revision(const ResonateDomDocument* document);

/* --- strings --- */

ResonateDomString resonate_dom_intern(ResonateDomDocument* document, const char* utf8,
                                      uint32_t size_bytes);
/* Borrowed bytes, NUL-terminated; null for the null string. */
const char* resonate_dom_text(const ResonateDomDocument* document, ResonateDomString string,
                              uint32_t* out_size);

/* --- nodes --- */

ResonateDomNode resonate_dom_root(const ResonateDomDocument* document);
ResonateStatus resonate_dom_set_root(ResonateDomDocument* document, ResonateDomNode node);
int32_t resonate_dom_alive(const ResonateDomDocument* document, ResonateDomNode node);

ResonateDomNode resonate_dom_parent(const ResonateDomDocument* document, ResonateDomNode node);
ResonateDomNode resonate_dom_first_child(const ResonateDomDocument* document,
                                         ResonateDomNode node);
ResonateDomNode resonate_dom_last_child(const ResonateDomDocument* document,
                                        ResonateDomNode node);
ResonateDomNode resonate_dom_next_sibling(const ResonateDomDocument* document,
                                          ResonateDomNode node);
ResonateDomNode resonate_dom_previous_sibling(const ResonateDomDocument* document,
                                              ResonateDomNode node);
uint32_t resonate_dom_child_count(const ResonateDomDocument* document, ResonateDomNode node);

ResonateDomNode resonate_dom_create_node(ResonateDomDocument* document, ResonateDomString type);
ResonateStatus resonate_dom_destroy_node(ResonateDomDocument* document, ResonateDomNode node);
ResonateStatus resonate_dom_detach(ResonateDomDocument* document, ResonateDomNode node);
ResonateStatus resonate_dom_append(ResonateDomDocument* document, ResonateDomNode parent,
                                   ResonateDomNode child);
ResonateStatus resonate_dom_insert_before(ResonateDomDocument* document, ResonateDomNode parent,
                                          ResonateDomNode child, ResonateDomNode reference);
ResonateStatus resonate_dom_insert_after(ResonateDomDocument* document, ResonateDomNode parent,
                                         ResonateDomNode child, ResonateDomNode reference);
ResonateStatus resonate_dom_replace(ResonateDomDocument* document, ResonateDomNode parent,
                                    ResonateDomNode old_child, ResonateDomNode new_child);
ResonateDomNode resonate_dom_clone(ResonateDomDocument* document, ResonateDomNode node,
                                   int32_t copy_key, int32_t copy_id);

/* --- identity --- */

ResonateDomString resonate_dom_key(const ResonateDomDocument* document, ResonateDomNode node);
ResonateStatus resonate_dom_set_key(ResonateDomDocument* document, ResonateDomNode node,
                                    ResonateDomString key);
ResonateDomString resonate_dom_id(const ResonateDomDocument* document, ResonateDomNode node);
ResonateStatus resonate_dom_set_id(ResonateDomDocument* document, ResonateDomNode node,
                                   ResonateDomString id);
ResonateDomString resonate_dom_name(const ResonateDomDocument* document, ResonateDomNode node);
ResonateStatus resonate_dom_set_name(ResonateDomDocument* document, ResonateDomNode node,
                                     ResonateDomString name);
ResonateDomString resonate_dom_type(const ResonateDomDocument* document, ResonateDomNode node);
ResonateStatus resonate_dom_set_type(ResonateDomDocument* document, ResonateDomNode node,
                                     ResonateDomString type);

uint32_t resonate_dom_class_count(const ResonateDomDocument* document, ResonateDomNode node);
ResonateDomString resonate_dom_class_at(const ResonateDomDocument* document, ResonateDomNode node,
                                        uint32_t index);
ResonateStatus resonate_dom_set_classes(ResonateDomDocument* document, ResonateDomNode node,
                                        const ResonateDomString* classes, uint32_t count);

/* --- components --- */

uint32_t resonate_dom_component_count(const ResonateDomDocument* document,
                                      ResonateDomNode node);
ResonateDomString resonate_dom_component_type_at(const ResonateDomDocument* document,
                                                 ResonateDomNode node, uint32_t index);
uint32_t resonate_dom_field_count(const ResonateDomDocument* document, ResonateDomNode node,
                                  ResonateDomString component);
ResonateDomString resonate_dom_field_name_at(const ResonateDomDocument* document,
                                             ResonateDomNode node, ResonateDomString component,
                                             uint32_t index);
const char* resonate_dom_field_value_at(const ResonateDomDocument* document, ResonateDomNode node,
                                        ResonateDomString component, uint32_t index,
                                        uint32_t* out_size);
ResonateStatus resonate_dom_set_field(ResonateDomDocument* document, ResonateDomNode node,
                                      ResonateDomString component, ResonateDomString field,
                                      const char* value, uint32_t size_bytes);
ResonateStatus resonate_dom_remove_component(ResonateDomDocument* document, ResonateDomNode node,
                                             ResonateDomString component);

/* --- authoring properties --- */

uint32_t resonate_dom_property_count(const ResonateDomDocument* document, ResonateDomNode node);
ResonateDomString resonate_dom_property_name_at(const ResonateDomDocument* document,
                                                ResonateDomNode node, uint32_t index);
const char* resonate_dom_property_value_at(const ResonateDomDocument* document,
                                           ResonateDomNode node, uint32_t index,
                                           uint32_t* out_size);
ResonateStatus resonate_dom_set_property(ResonateDomDocument* document, ResonateDomNode node,
                                         ResonateDomString name, const char* value,
                                         uint32_t size_bytes);
ResonateStatus resonate_dom_remove_property(ResonateDomDocument* document, ResonateDomNode node,
                                            ResonateDomString name);

/* --- queries --- */

ResonateDomNode resonate_dom_find_key(const ResonateDomDocument* document, ResonateDomString key);
ResonateDomNode resonate_dom_find_id(const ResonateDomDocument* document, ResonateDomString id);
uint32_t resonate_dom_find_type_count(const ResonateDomDocument* document, ResonateDomString type);
uint32_t resonate_dom_find_type_fill(const ResonateDomDocument* document, ResonateDomString type,
                                     ResonateDomNode* out_nodes, uint32_t capacity);
uint32_t resonate_dom_find_class_count(const ResonateDomDocument* document,
                                       ResonateDomString class_id);
uint32_t resonate_dom_find_class_fill(const ResonateDomDocument* document,
                                      ResonateDomString class_id, ResonateDomNode* out_nodes,
                                      uint32_t capacity);

#ifdef __cplusplus
} // extern "C"
#endif

#endif /* RESONATE_DOM_ABI_H */
