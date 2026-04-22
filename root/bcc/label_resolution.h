#ifndef LABEL_RESOLUTION_H
#define LABEL_RESOLUTION_H

#include "AST.h"
#include "../crt/stdbool.h"

// Purpose: Defines the loop/switch labeling pass for control-flow constructs.
// Inputs: Operates on AST nodes produced by the parser and identifier resolver.
// Outputs: Annotates statements with labels and case lists.
// Invariants/Assumptions: AST nodes are arena-allocated and labels are slices.

// Purpose: Distinguish the current control-flow construct for break/continue.
// Inputs: Stored in label-resolution state while traversing statements.
// Outputs: Guides label assignment for break/continue.
// Invariants/Assumptions: Only LOOP and SWITCH are tracked at a time.
enum LabelType {
  LOOP,
  SWITCH
};

// Purpose: Assign unique labels for loops/switches and resolve gotos in a program.
// Inputs: prog is the parsed Program AST.
// Outputs: Returns true on success; errors are reported to stdout on failure.
// Invariants/Assumptions: Function bodies are labeled independently.
bool label_loops(struct Program* prog);

// Purpose: Label a single statement subtree for control-flow and gotos.
// Inputs: func_name is the enclosing function name; stmt is the statement node.
// Outputs: Returns true on success; emits errors on failure.
// Invariants/Assumptions: Label state is managed by the caller for nesting.
bool label_stmt(struct Slice* func_name, struct Statement* stmt);

// Purpose: Label each statement within a block.
// Inputs: func_name is the enclosing function name; block is the block list.
// Outputs: Returns true on success; emits errors on failure.
// Invariants/Assumptions: Block items are either statements or declarations.
bool label_block(struct Slice* func_name, struct Block* block);

// Purpose: Resolve goto targets to the unique labels assigned to definitions.
// Inputs: block is the block list containing goto statements.
// Outputs: Returns true on success; errors are reported on unresolved labels.
// Invariants/Assumptions: label_loops has already populated goto label map.
bool resolve_gotos(struct Block* block);

// Purpose: Collect case/default labels for each switch statement.
// Inputs: block is the block list containing switch statements.
// Outputs: Returns true on success; errors are reported on duplicates.
// Invariants/Assumptions: Case expressions must be literals.
bool collect_cases(struct Block* block);

// Purpose: Build a unique case label string for a switch and case value.
// Inputs: switch_label is the switch's unique label; case_value is the literal.
// Outputs: Returns a newly allocated Slice for the case label.
// Invariants/Assumptions: Uses arena allocation for the label buffer.
struct Slice* make_case_label(struct Slice* switch_label, int case_value);

#endif // LABEL_RESOLUTION_H
