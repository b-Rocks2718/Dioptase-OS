#ifndef LABEL_RESOLUTION_H
#define LABEL_RESOLUTION_H

#include "AST.h"
#include "../crt/stdbool.h"

// Defines the loop/switch labeling pass for control-flow constructs.
// Annotates statements with labels and case lists.

// Distinguish the current control-flow construct for break/continue.
// Guides label assignment for break/continue.
enum LabelType {
  LOOP,
  SWITCH
};

// Assign unique labels for loops/switches and resolve gotos in a program.
// Returns true on success; errors are reported to stdout on failure.
bool label_loops(struct Program* prog);

// Label a single statement subtree for control-flow and gotos.
// Returns true on success; emits errors on failure.
bool label_stmt(struct Slice* func_name, struct Statement* stmt);

// Label each statement within a block.
// Returns true on success; emits errors on failure.
bool label_block(struct Slice* func_name, struct Block* block);

// Resolve goto targets to the unique labels assigned to definitions.
// Returns true on success; errors are reported on unresolved labels.
bool resolve_gotos(struct Block* block);

// Collect case/default labels for each switch statement.
// Returns true on success; errors are reported on duplicates.
// Case expressions must be literals.
bool collect_cases(struct Block* block);

// Build a unique case label string for a switch and case value.
// Returns a newly allocated Slice for the case label.
struct Slice* make_case_label(struct Slice* switch_label, int case_value);

#endif // LABEL_RESOLUTION_H
