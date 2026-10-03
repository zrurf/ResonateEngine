#include <catch2/catch_all.hpp>

#include <string>
#include <vector>

#include <resonate/core/allocator.h>
#include <resonate/ecs/command_buffer.h>
#include <resonate/ecs/world.h>
#include <resonate/hierarchy/tree.h>

namespace
{

using resonate::ecs::CommandBuffer;
using resonate::ecs::Entity;
using resonate::ecs::World;
using namespace resonate::hierarchy;

/* What the world reports is only observable through its sink. */
struct Reports
{
    std::vector<std::string> lines;

    static void sink(void* user_data, World::Report, const char* message)
    {
        static_cast<Reports*>(user_data)->lines.emplace_back(message != nullptr ? message : "");
    }

    [[nodiscard]] bool contains(const char* fragment) const
    {
        for (const std::string& line : lines)
        {
            if (line.find(fragment) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }

    void clear()
    {
        lines.clear();
    }
};

struct Tree
{
    World world;
    Reports reports;

    Tree() : world(resonate::systemAllocator())
    {
        world.setReportSink(&reports, &Reports::sink);
        REQUIRE(registerComponents(world));
    }

    [[nodiscard]] Entity spawn(CommandBuffer& commands)
    {
        return commands.create();
    }

    [[nodiscard]] bool isChildOf(Entity child, Entity parent) const
    {
        if (parentOf(world, child) != parent)
        {
            return false;
        }
        for (const Entity listed : children(world, parent))
        {
            if (listed == child)
            {
                return true;
            }
        }
        return false;
    }
};

} // namespace

TEST_CASE("set-parent builds both sides of the relation", "[hierarchy][tree]")
{
    Tree tree;
    CommandBuffer commands(tree.world);

    const Entity root = tree.spawn(commands);
    const Entity first = tree.spawn(commands);
    const Entity second = tree.spawn(commands);
    setParent(commands, first, root);
    setParent(commands, second, root);

    /* Reserved handles are attachable before playback: the ops play in record
       order, after the creates. */
    REQUIRE_FALSE(tree.world.alive(root));
    tree.world.play(commands);

    REQUIRE(tree.world.alive(root));
    REQUIRE(tree.isChildOf(first, root));
    REQUIRE(tree.isChildOf(second, root));
    REQUIRE(childCount(tree.world, root) == 2);
    REQUIRE(parentOf(tree.world, root) == Entity{});
    REQUIRE(childCount(tree.world, first) == 0);

    /* Insertion order is kept. */
    const auto listed = children(tree.world, root);
    REQUIRE(listed.size() == 2);
    REQUIRE(listed[0] == first);
    REQUIRE(listed[1] == second);
    REQUIRE(isAncestorOf(tree.world, root, first));
    REQUIRE_FALSE(isAncestorOf(tree.world, first, root));
}

TEST_CASE("reparenting moves the child between lists", "[hierarchy][tree]")
{
    Tree tree;
    CommandBuffer commands(tree.world);

    const Entity first = tree.spawn(commands);
    const Entity second = tree.spawn(commands);
    const Entity child = tree.spawn(commands);
    setParent(commands, child, first);
    setParent(commands, child, second);
    setParent(commands, second, first);
    tree.world.play(commands);

    REQUIRE(tree.isChildOf(child, second));
    REQUIRE(tree.isChildOf(second, first));
    REQUIRE(childCount(tree.world, first) == 1);

    /* And the other way: the child goes back under the first parent. */
    setParent(commands, child, first);
    tree.world.play(commands);
    REQUIRE(tree.isChildOf(child, first));
    REQUIRE(childCount(tree.world, second) == 0);

    /* The emptied parent loses the component: has<Children> is the invariant
       "has children". */
    REQUIRE_FALSE(tree.world.has<Children>(second));
    REQUIRE(tree.world.has<Children>(first));
}

TEST_CASE("clear-parent makes a root and empties the parent's list", "[hierarchy][tree]")
{
    Tree tree;
    CommandBuffer commands(tree.world);

    const Entity parent = tree.spawn(commands);
    const Entity child = tree.spawn(commands);
    setParent(commands, child, parent);
    tree.world.play(commands);
    REQUIRE(tree.world.has<Children>(parent));

    clearParent(commands, child);
    tree.world.play(commands);
    REQUIRE(parentOf(tree.world, child) == Entity{});
    REQUIRE_FALSE(tree.world.has<Children>(parent));
    REQUIRE_FALSE(tree.world.has<Children>(child));
    REQUIRE(tree.world.blobCount() == 0);
    REQUIRE(tree.world.alive(parent));
    REQUIRE(tree.world.alive(child));

    /* Clearing a root is a no-op, not a report. */
    tree.reports.clear();
    clearParent(commands, parent);
    tree.world.play(commands);
    REQUIRE(tree.reports.lines.empty());
}

TEST_CASE("a cycle is rejected at playback and nothing moves", "[hierarchy][tree]")
{
    Tree tree;
    CommandBuffer commands(tree.world);

    const Entity root = tree.spawn(commands);
    const Entity middle = tree.spawn(commands);
    const Entity leaf = tree.spawn(commands);
    setParent(commands, middle, root);
    setParent(commands, leaf, middle);
    tree.world.play(commands);

    /* root -> middle -> leaf, so putting root under leaf folds the tree. */
    setParent(commands, root, leaf);
    tree.world.play(commands);

    REQUIRE(tree.reports.contains("would create a cycle"));
    REQUIRE(parentOf(tree.world, root) == Entity{});
    REQUIRE(tree.isChildOf(middle, root));
    REQUIRE(tree.isChildOf(leaf, middle));
    REQUIRE(childCount(tree.world, leaf) == 0);

    /* A self-parent is refused where it is recorded, before anything is
       written. */
    tree.reports.clear();
    setParent(commands, middle, middle);
    REQUIRE(commands.empty());
    REQUIRE(tree.reports.contains("two different live handles"));
}

TEST_CASE("the depth limit is enforced against the moved subtree", "[hierarchy][tree]")
{
    Tree tree;
    CommandBuffer commands(tree.world);

    /* A chain of kMaxDepth + 1 nodes sits exactly at the limit. */
    std::vector<Entity> chain;
    Entity previous{};
    for (std::uint32_t depth = 0; depth <= kMaxDepth; ++depth)
    {
        const Entity node = tree.spawn(commands);
        if (previous.valid())
        {
            setParent(commands, node, previous);
        }
        chain.push_back(node);
        previous = node;
    }
    tree.world.play(commands);
    REQUIRE_FALSE(tree.reports.contains("past"));
    REQUIRE(isAncestorOf(tree.world, chain[0], chain[kMaxDepth]));

    /* One more under the deepest node exceeds it. */
    const Entity extra = tree.spawn(commands);
    setParent(commands, extra, chain[kMaxDepth]);
    tree.world.play(commands);
    REQUIRE(tree.reports.contains("past"));
    REQUIRE(parentOf(tree.world, extra) == Entity{});

    /* Moving a deep subtree under a fresh root does not help: the subtree's own
       height counts against the limit. */
    tree.reports.clear();
    const Entity side = tree.spawn(commands);
    setParent(commands, chain[0], side);
    tree.world.play(commands);
    REQUIRE(tree.reports.contains("would push the subtree past"));
    REQUIRE(parentOf(tree.world, chain[0]) == Entity{});
}

TEST_CASE("rejected moves leave the tree untouched and report", "[hierarchy][tree]")
{
    Tree tree;
    CommandBuffer commands(tree.world);

    const Entity parent = tree.spawn(commands);
    const Entity child = tree.spawn(commands);
    setParent(commands, child, parent);
    tree.world.play(commands);

    /* Recording refuses the obviously wrong handles here and now. */
    tree.reports.clear();
    setParent(commands, child, child);
    setParent(commands, Entity{}, parent);
    REQUIRE(commands.empty());
    REQUIRE(tree.reports.contains("two different live handles"));

    /* A dead end reports at playback. */
    tree.reports.clear();
    const Entity dead = tree.spawn(commands);
    tree.world.play(commands);
    tree.world.destroy(dead);

    setParent(commands, child, dead);
    tree.world.play(commands);
    REQUIRE(tree.reports.contains("not alive at playback"));
    REQUIRE(tree.isChildOf(child, parent));

    /* A detached clear of an unknown handle reports too. */
    tree.reports.clear();
    clearParent(commands, dead);
    tree.world.play(commands);
    REQUIRE(tree.reports.contains("clearParent: entity"));
}

TEST_CASE("destroyEntity detaches the children and forgets the parent", "[hierarchy][tree]")
{
    Tree tree;
    CommandBuffer commands(tree.world);

    const Entity root = tree.spawn(commands);
    const Entity middle = tree.spawn(commands);
    const Entity leaf = tree.spawn(commands);
    const Entity sibling = tree.spawn(commands);
    setParent(commands, middle, root);
    setParent(commands, leaf, middle);
    setParent(commands, sibling, root);
    tree.world.play(commands);
    REQUIRE(tree.world.blobCount() == 2);

    destroyEntity(commands, middle);
    tree.world.play(commands);

    REQUIRE_FALSE(tree.world.alive(middle));
    /* The leaf survives as a root: destroying a node does not silently delete
       the subtree. */
    REQUIRE(tree.world.alive(leaf));
    REQUIRE(parentOf(tree.world, leaf) == Entity{});
    /* The parent's list no longer names the dead handle. */
    REQUIRE(childCount(tree.world, root) == 1);
    REQUIRE(children(tree.world, root)[0] == sibling);
    REQUIRE(tree.world.blobCount() == 1);
    REQUIRE_FALSE(tree.reports.contains("stale"));
}

TEST_CASE("destroying a child through commands.destroy leaves a stale handle", "[hierarchy][tree]")
{
    Tree tree;
    CommandBuffer commands(tree.world);

    const Entity parent = tree.spawn(commands);
    const Entity child = tree.spawn(commands);
    setParent(commands, child, parent);
    tree.world.play(commands);

    /* The trap the tree command exists to close: a plain destroy is not a tree
       edit, so the parent still lists the handle. Reads report the dead entry
       rather than dropping it silently. */
    commands.destroy(child);
    tree.world.play(commands);

    const auto listed = children(tree.world, parent);
    REQUIRE(listed.size() == 1);
    REQUIRE(listed[0] == child);
    REQUIRE_FALSE(tree.world.alive(listed[0]));
}

TEST_CASE("reads answer safely for entities without a place in the tree", "[hierarchy][tree]")
{
    Tree tree;
    CommandBuffer commands(tree.world);

    const Entity lonely = tree.spawn(commands);
    tree.world.play(commands);
    tree.world.destroy(lonely);

    REQUIRE(parentOf(tree.world, lonely) == Entity{});
    REQUIRE(children(tree.world, lonely).empty());
    REQUIRE(childCount(tree.world, lonely) == 0);
    REQUIRE_FALSE(isAncestorOf(tree.world, lonely, lonely));
    REQUIRE_FALSE(isAncestorOf(tree.world, Entity{}, Entity{}));
}
