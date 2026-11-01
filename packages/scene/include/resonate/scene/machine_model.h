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

struct MachineModel
{
    std::string entry;
    std::string parameters; /* space separated, as authored */
    std::vector<MachineModelState> states;
    std::vector<MachineModelTransition> transitions;
};

/* Parses the authored <Machine> element (a single root). Unknown elements,
   missing names or clips, duplicate state names, and malformed numbers in
   condition values are errors with a line number. */
[[nodiscard]] bool parseMachineFragment(std::string_view xml, MachineModel& out,
                                        std::string& error);

/* Serializes the model back to the element: only the attributes present,
   values escaped, conditions nested inside their transition. Two
   serializations of one model are byte-identical. */
[[nodiscard]] std::string serializeMachineFragment(const MachineModel& model);

} // namespace resonate::scene

#endif /* RESONATE_SCENE_MACHINE_MODEL_H */
