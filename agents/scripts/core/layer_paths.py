"""layer_paths.py - a layer path spelled the way the scanned project's checkout sees it.

The audits run with the project as their working directory. Before the flip, and
in a standalone layer checkout, the project is the layer itself, so a layer path
is spelled as is. After it the layer is the project's agent-layer/ submodule
(plan agent-surface-extraction-repo, row 12), and a regeneration command printed
into a host baseline has to say `agent-layer/agents/...`, or it names a file
that is not there.
"""

import os

# This file is <layer>/agents/scripts/core/layer_paths.py.
_LAYER_ROOT = os.path.dirname(
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
)


# The mount's name is fixed by .gitmodules (row 11b) and is the name row 12's
# marker test uses.
MOUNT = "agent-layer"


def from_project(rel, project_root=None):
    """Return `rel`, a path inside the layer, as seen from `project_root`.

    `project_root` defaults to the working directory. `rel` is prefixed with
    `agent-layer/` only when this layer is the project's agent-layer/ mount. A
    layer that is the project itself, an unrelated project (a test fixture), and
    a layer that merely sits somewhere below the project (a nested worktree under
    .claude/worktrees/) all get `rel` unchanged.
    """
    try:
        mount = os.path.relpath(
            os.path.realpath(_LAYER_ROOT), os.path.realpath(project_root or os.getcwd())
        )
    except ValueError:  # different drives on Windows: the layer is not inside
        return rel
    if mount.replace(os.sep, "/") != MOUNT:
        return rel
    return MOUNT + "/" + rel
