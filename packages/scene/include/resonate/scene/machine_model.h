#ifndef RESONATE_SCENE_MACHINE_MODEL_H
#define RESONATE_SCENE_MACHINE_MODEL_H

/*
 * The visual state machine editor's model over an authored <Machine> fragment:
 * the same grammar the scene adapter parses, held as authored strings so an
 * editor can edit what the document said without knowing how the player builds
 * runtime definitions from it. parse and serialize are inverse enough that a
 * serialize -> parse round trip answers the same model, and two serializations
 * of one model are byte-identical.
 */

#include <string>
#include <string_view>
#include <vector>

namespace resonate::scene
{

struct MachineModelState
{
    std::string name;
    std::string clip; /* "@guid[:sub]" or "#alias" as authored */
    std::string looping; /* "true"/"false" as authored; empty takes the default */
    /* The visual editor's layout hints, authored and written back verbatim;
       empty when the document never positioned the state. */
    std::string x;
    std::string y;
};

struct MachineModelCondition
{
    std::string parameter;
    /* The authored test: "greater", "less", "equal", "not_equal", "trigger"
       or "is"; the value text belongs to it. */
    std::string test;
    std::string value;
};

struct MachineModelTransition
{
    std::string from;
    std::string to;
    std::string at_end; /* "true"/"false" as authored; empty takes the default */
    std::string fade;
    std::string sync;
    std::vector<MachineModelCondition> conditions;
};

struct MachineModelResource
{
    std::string key;   /* the alias a clip names, "#key" */
    std::string asset; /* "@guid[:sub]" as authored */
};

struct MachineModel
{
    std::string entry;
    std::string parameters; /* space separated, as authored */
    std::vector<MachineModelState> states;
    std::vector<MachineModelTransition> transitions;
    /* Resource rows the machine's clips resolved through, kept so a capture and
       the editor can write the fragment back without losing the aliases. */
    std::vector<MachineModelResource> resources;
};

/* Parses the authored fragment: optional <Resource/> rows, then the <Machine>
   element. Unknown elements, missing names or clips, duplicate state names,
   and malformed numbers in condition values are errors with a line number. */
[[nodiscard]] bool parseMachineFragment(std::string_view xml, MachineModel& out,
                                        std::string& error);

/* Serializes the model back to the element: only the attributes present,
   values escaped, conditions nested inside their transition. Two
   serializations of one model are byte-identical. */
[[nodiscard]] std::string serializeMachineFragment(const MachineModel& model);

/* The editor's one-line condition spec, `<parameter> <test> <value>`, and its
   parser. The parser accepts the four comparisons (a numeric value), `trigger`
   (no value) and `is` (true/false); anything else is refused with a reason. */
[[nodiscard]] std::string formatConditionSpec(const MachineModelCondition& condition);
[[nodiscard]] bool parseConditionSpec(std::string_view text, MachineModelCondition& out,
                                      std::string& error);

} // namespace resonate::scene

#endif /* RESONATE_SCENE_MACHINE_MODEL_H */
