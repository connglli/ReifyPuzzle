import os

# Define the directories to create/write tests to
TEST_DIRS = {
  "lexer": "test/lexer",
  "parser": "test/parser",
  "typechecker": "test/typechecker",
  "semchecker": "test/semchecker",
  "interp": "test/interp",
  "compile": "test/compile",
  "solver": "test/solver",
}

# Ensure all directories exist
for d in TEST_DIRS.values():
  os.makedirs(d, exist_ok=True)


# Helper to ensure a block has at least 3 instructions by backfilling with identity operations
def backfill_block(instrs, is_fp=False):
  # Ensure instrs is a list
  if isinstance(instrs, str):
    instrs = [i.strip() for i in instrs.strip().split("\n") if i.strip()]

  # Backfill with vector identity updates if under 3 instructions
  while len(instrs) < 3:
    if is_fp:
      instrs.append("%f_val = 1.0 * %f_val;")
    else:
      instrs.append("%v_val = 1 * %v_val;")
  return "\n  ".join(instrs)


# Helper to partition locals into syms and lets
def partition_locals(locals_str):
  if not locals_str:
    return "", ""
  parts = locals_str.split(";")
  syms = []
  lets = []
  for p in parts:
    p_clean = p.strip()
    if not p_clean:
      continue
    if p_clean.startswith("sym "):
      syms.append(p_clean + ";")
    else:
      lets.append(p_clean + ";")
  return "\n  ".join(syms), "\n  ".join(lets)


# Topology A: Front-Loaded Loop (10+ Blocks)
def make_topology_a(scenario, is_fp=False, header=""):
  locals_str = scenario.get("locals", "")
  is_fp_scen = is_fp or "f32" in locals_str or "f64" in locals_str
  syms_str, lets_str = partition_locals(locals_str)

  b_entry = backfill_block(scenario.get("entry", ""), is_fp_scen)
  b_loop_cond = backfill_block(scenario.get("loop_cond", ""), is_fp_scen)
  b_body1 = backfill_block(scenario.get("body1", ""), is_fp_scen)
  b_body2 = backfill_block(scenario.get("body2", ""), is_fp_scen)
  b_body3 = backfill_block(scenario.get("body3", ""), is_fp_scen)
  b_body4 = backfill_block(scenario.get("body4", ""), is_fp_scen)
  b_latch = backfill_block(scenario.get("latch", ""), is_fp_scen)
  b_exit = backfill_block(scenario.get("exit", ""), is_fp_scen)
  b_post1 = backfill_block(scenario.get("post1", ""), is_fp_scen)
  b_post2 = backfill_block(scenario.get("post2", ""), is_fp_scen)
  b_post3 = backfill_block(scenario.get("post3", ""), is_fp_scen)

  return f"""{header}
fun @main() : i32 {{
  {syms_str}
  let mut %loop_idx: i32 = 0;
  let %two: i32 = 2;
  let mut %v_val: <4> i32 = {{1, 2, 3, 4}};
  let mut %f_val: <4> f32 = {{1.0, 2.0, 3.0, 4.0}};
  let mut %cond: i1 = 0;
  let mut %cond2: i1 = 0;
  let %one_i32: i32 = 1;
  let %two_i32: i32 = 2;
  let %three_i32: i32 = 3;
  let %four_i32: i32 = 4;
  let %zero_i32: i32 = 0;
  let %one_f32: f32 = 1.0;
  let %two_f32: f32 = 2.0;
  let %three_f32: f32 = 3.0;
  let %four_f32: f32 = 4.0;
  let %zero_f32: f32 = 0.0;
  {lets_str}

^entry:
  {b_entry}
  br ^loop_cond;

^loop_cond:
  {b_loop_cond}
  br %cond != 0, ^body1, ^exit;

^body1:
  {b_body1}
  br ^body2;

^body2:
  {b_body2}
  br ^body3;

^body3:
  {b_body3}
  br ^body4;

^body4:
  {b_body4}
  br ^latch;

^latch:
  {b_latch}
  br ^loop_cond;

^exit:
  {b_exit}
  br ^post1;

^post1:
  {b_post1}
  br ^post2;

^post2:
  {b_post2}
  br ^post3;

^post3:
  {b_post3}
  ret 0;
}}
"""


# Topology B: Back-Loaded Loop (12 Blocks)
def make_topology_b(scenario, is_fp=False, header=""):
  locals_str = scenario.get("locals", "")
  is_fp_scen = is_fp or "f32" in locals_str or "f64" in locals_str
  syms_str, lets_str = partition_locals(locals_str)

  b_entry = backfill_block(scenario.get("entry", ""), is_fp_scen)
  b_pre1 = backfill_block(scenario.get("pre1", ""), is_fp_scen)
  b_pre2 = backfill_block(scenario.get("pre2", ""), is_fp_scen)
  b_pre3 = backfill_block(scenario.get("pre3", ""), is_fp_scen)
  b_pre4 = backfill_block(scenario.get("pre4", ""), is_fp_scen)
  b_pre5 = backfill_block(scenario.get("pre5", ""), is_fp_scen)
  b_pre6 = backfill_block(scenario.get("pre6", ""), is_fp_scen)
  b_loop_cond = backfill_block(scenario.get("loop_cond", ""), is_fp_scen)
  b_body1 = backfill_block(scenario.get("body1", ""), is_fp_scen)
  b_body2 = backfill_block(scenario.get("body2", ""), is_fp_scen)
  b_body3 = backfill_block(scenario.get("body3", ""), is_fp_scen)
  b_latch = backfill_block(scenario.get("latch", ""), is_fp_scen)
  b_exit = backfill_block(scenario.get("exit", ""), is_fp_scen)

  return f"""{header}
fun @main() : i32 {{
  {syms_str}
  let mut %loop_idx: i32 = 0;
  let %two: i32 = 2;
  let mut %v_val: <4> i32 = {{1, 2, 3, 4}};
  let mut %f_val: <4> f32 = {{1.0, 2.0, 3.0, 4.0}};
  let mut %cond: i1 = 0;
  let mut %cond2: i1 = 0;
  let %one_i32: i32 = 1;
  let %two_i32: i32 = 2;
  let %three_i32: i32 = 3;
  let %four_i32: i32 = 4;
  let %zero_i32: i32 = 0;
  let %one_f32: f32 = 1.0;
  let %two_f32: f32 = 2.0;
  let %three_f32: f32 = 3.0;
  let %four_f32: f32 = 4.0;
  let %zero_f32: f32 = 0.0;
  {lets_str}

^entry:
  {b_entry}
  br ^pre1;

^pre1:
  {b_pre1}
  br ^pre2;

^pre2:
  {b_pre2}
  br ^pre3;

^pre3:
  {b_pre3}
  br ^pre4;

^pre4:
  {b_pre4}
  br ^pre5;

^pre5:
  {b_pre5}
  br ^pre6;

^pre6:
  {b_pre6}
  br ^loop_cond;

^loop_cond:
  {b_loop_cond}
  br %cond != 0, ^body1, ^exit;

^body1:
  {b_body1}
  br ^body2;

^body2:
  {b_body2}
  br ^body3;

^body3:
  {b_body3}
  br ^latch;

^latch:
  {b_latch}
  br ^loop_cond;

^exit:
  {b_exit}
  ret 0;
}}
"""


# Topology C: Loop with Internal Branching (13 Blocks)
def make_topology_c(scenario, is_fp=False, header=""):
  locals_str = scenario.get("locals", "")
  is_fp_scen = is_fp or "f32" in locals_str or "f64" in locals_str
  syms_str, lets_str = partition_locals(locals_str)

  b_entry = backfill_block(scenario.get("entry", ""), is_fp_scen)
  b_pre1 = backfill_block(scenario.get("pre1", ""), is_fp_scen)
  b_pre2 = backfill_block(scenario.get("pre2", ""), is_fp_scen)
  b_loop_cond = backfill_block(scenario.get("loop_cond", ""), is_fp_scen)
  b_body_start = backfill_block(scenario.get("body_start", ""), is_fp_scen)
  b_body_then = backfill_block(scenario.get("body_then", ""), is_fp_scen)
  b_body_else = backfill_block(scenario.get("body_else", ""), is_fp_scen)
  b_body_merge = backfill_block(scenario.get("body_merge", ""), is_fp_scen)
  b_latch = backfill_block(scenario.get("latch", ""), is_fp_scen)
  b_exit = backfill_block(scenario.get("exit", ""), is_fp_scen)
  b_post1 = backfill_block(scenario.get("post1", ""), is_fp_scen)

  return f"""{header}
fun @main() : i32 {{
  {syms_str}
  let mut %loop_idx: i32 = 0;
  let %two: i32 = 2;
  let mut %v_val: <4> i32 = {{1, 2, 3, 4}};
  let mut %f_val: <4> f32 = {{1.0, 2.0, 3.0, 4.0}};
  let mut %cond: i1 = 0;
  let mut %cond2: i1 = 0;
  let mut %branch_cond: i1 = 0;
  let %one_i32: i32 = 1;
  let %two_i32: i32 = 2;
  let %three_i32: i32 = 3;
  let %four_i32: i32 = 4;
  let %zero_i32: i32 = 0;
  let %one_f32: f32 = 1.0;
  let %two_f32: f32 = 2.0;
  let %three_f32: f32 = 3.0;
  let %four_f32: f32 = 4.0;
  let %zero_f32: f32 = 0.0;
  {lets_str}

^entry:
  {b_entry}
  br ^pre1;

^pre1:
  {b_pre1}
  br ^pre2;

^pre2:
  {b_pre2}
  br ^loop_cond;

^loop_cond:
  {b_loop_cond}
  br %cond != 0, ^body_start, ^exit;

^body_start:
  {b_body_start}
  br %branch_cond != 0, ^body_then, ^body_else;

^body_then:
  {b_body_then}
  br ^body_merge;

^body_else:
  {b_body_else}
  br ^body_merge;

^body_merge:
  {b_body_merge}
  br ^latch;

^latch:
  {b_latch}
  br ^loop_cond;

^exit:
  {b_exit}
  br ^post1;

^post1:
  {b_post1}
  ret 0;
}}
"""


# Topology D: Nested Loops (15 Blocks)
def make_topology_d(scenario, is_fp=False, header=""):
  locals_str = scenario.get("locals", "")
  is_fp_scen = is_fp or "f32" in locals_str or "f64" in locals_str
  syms_str, lets_str = partition_locals(locals_str)

  b_entry = backfill_block(scenario.get("entry", ""), is_fp_scen)
  b_outer_cond = backfill_block(scenario.get("outer_cond", ""), is_fp_scen)
  b_outer_body1 = backfill_block(scenario.get("outer_body1", ""), is_fp_scen)
  b_inner_cond = backfill_block(scenario.get("inner_cond", ""), is_fp_scen)
  b_inner_body1 = backfill_block(scenario.get("inner_body1", ""), is_fp_scen)
  b_inner_body2 = backfill_block(scenario.get("inner_body2", ""), is_fp_scen)
  b_inner_latch = backfill_block(scenario.get("inner_latch", ""), is_fp_scen)
  b_outer_latch = backfill_block(scenario.get("outer_latch", ""), is_fp_scen)
  b_exit = backfill_block(scenario.get("exit", ""), is_fp_scen)
  b_post1 = backfill_block(scenario.get("post1", ""), is_fp_scen)
  b_post2 = backfill_block(scenario.get("post2", ""), is_fp_scen)
  b_post3 = backfill_block(scenario.get("post3", ""), is_fp_scen)

  return f"""{header}
fun @main() : i32 {{
  {syms_str}
  let mut %loop_idx: i32 = 0;
  let mut %inner_idx: i32 = 0;
  let %two: i32 = 2;
  let mut %v_val: <4> i32 = {{1, 2, 3, 4}};
  let mut %f_val: <4> f32 = {{1.0, 2.0, 3.0, 4.0}};
  let mut %cond: i1 = 0;
  let mut %cond2: i1 = 0;
  let mut %inner_cond_val: i1 = 0;
  let %one_i32: i32 = 1;
  let %two_i32: i32 = 2;
  let %three_i32: i32 = 3;
  let %four_i32: i32 = 4;
  let %zero_i32: i32 = 0;
  let %one_f32: f32 = 1.0;
  let %two_f32: f32 = 2.0;
  let %three_f32: f32 = 3.0;
  let %four_f32: f32 = 4.0;
  let %zero_f32: f32 = 0.0;
  {lets_str}

^entry:
  {b_entry}
  br ^outer_cond;

^outer_cond:
  {b_outer_cond}
  br %cond != 0, ^outer_body1, ^exit;

^outer_body1:
  {b_outer_body1}
  br ^inner_cond;

^inner_cond:
  {b_inner_cond}
  br %inner_cond_val != 0, ^inner_body1, ^outer_latch;

^inner_body1:
  {b_inner_body1}
  br ^inner_body2;

^inner_body2:
  {b_inner_body2}
  br ^inner_latch;

^inner_latch:
  {b_inner_latch}
  br ^inner_cond;

^outer_latch:
  {b_outer_latch}
  br ^outer_cond;

^exit:
  {b_exit}
  br ^post1;

^post1:
  {b_post1}
  br ^post2;

^post2:
  {b_post2}
  br ^post3;

^post3:
  {b_post3}
  ret 0;
}}
"""


# Topology E: Back-to-Back Loops (14 Blocks)
def make_topology_e(scenario, is_fp=False, header=""):
  locals_str = scenario.get("locals", "")
  is_fp_scen = is_fp or "f32" in locals_str or "f64" in locals_str
  syms_str, lets_str = partition_locals(locals_str)

  b_entry = backfill_block(scenario.get("entry", ""), is_fp_scen)
  b_l1_cond = backfill_block(scenario.get("l1_cond", ""), is_fp_scen)
  b_l1_body = backfill_block(scenario.get("l1_body", ""), is_fp_scen)
  b_l1_latch = backfill_block(scenario.get("l1_latch", ""), is_fp_scen)
  b_mid1 = backfill_block(scenario.get("mid1", ""), is_fp_scen)
  b_mid2 = backfill_block(scenario.get("mid2", ""), is_fp_scen)
  b_l2_cond = backfill_block(scenario.get("l2_cond", ""), is_fp_scen)
  b_l2_body = backfill_block(scenario.get("l2_body", ""), is_fp_scen)
  b_l2_latch = backfill_block(scenario.get("l2_latch", ""), is_fp_scen)
  b_exit = backfill_block(scenario.get("exit", ""), is_fp_scen)
  b_post1 = backfill_block(scenario.get("post1", ""), is_fp_scen)

  return f"""{header}
fun @main() : i32 {{
  {syms_str}
  let mut %loop_idx: i32 = 0;
  let mut %loop_idx2: i32 = 0;
  let %two: i32 = 2;
  let mut %v_val: <4> i32 = {{1, 2, 3, 4}};
  let mut %f_val: <4> f32 = {{1.0, 2.0, 3.0, 4.0}};
  let mut %cond: i1 = 0;
  let mut %cond2: i1 = 0;
  let %one_i32: i32 = 1;
  let %two_i32: i32 = 2;
  let %three_i32: i32 = 3;
  let %four_i32: i32 = 4;
  let %zero_i32: i32 = 0;
  let %one_f32: f32 = 1.0;
  let %two_f32: f32 = 2.0;
  let %three_f32: f32 = 3.0;
  let %four_f32: f32 = 4.0;
  let %zero_f32: f32 = 0.0;
  {lets_str}

^entry:
  {b_entry}
  br ^l1_cond;

^l1_cond:
  {b_l1_cond}
  br %cond != 0, ^l1_body, ^mid1;

^l1_body:
  {b_l1_body}
  br ^l1_latch;

^l1_latch:
  {b_l1_latch}
  br ^l1_cond;

^mid1:
  {b_mid1}
  br ^mid2;

^mid2:
  {b_mid2}
  br ^l2_cond;

^l2_cond:
  {b_l2_cond}
  br %cond2 != 0, ^l2_body, ^exit;

^l2_body:
  {b_l2_body}
  br ^l2_latch;

^l2_latch:
  {b_l2_latch}
  br ^l2_cond;

^exit:
  {b_exit}
  br ^post1;

^post1:
  {b_post1}
  ret 0;
}}
"""


# Topology F: Loop with Early Exit (11 Blocks)
def make_topology_f(scenario, is_fp=False, header=""):
  locals_str = scenario.get("locals", "")
  is_fp_scen = is_fp or "f32" in locals_str or "f64" in locals_str
  syms_str, lets_str = partition_locals(locals_str)

  b_entry = backfill_block(scenario.get("entry", ""), is_fp_scen)
  b_loop_cond = backfill_block(scenario.get("loop_cond", ""), is_fp_scen)
  b_body1 = backfill_block(scenario.get("body1", ""), is_fp_scen)
  b_body2 = backfill_block(scenario.get("body2", ""), is_fp_scen)
  b_latch = backfill_block(scenario.get("latch", ""), is_fp_scen)
  b_early_exit = backfill_block(scenario.get("early_exit", ""), is_fp_scen)
  b_exit = backfill_block(scenario.get("exit", ""), is_fp_scen)

  return f"""{header}
fun @main() : i32 {{
  {syms_str}
  let mut %loop_idx: i32 = 0;
  let %two: i32 = 2;
  let mut %v_val: <4> i32 = {{1, 2, 3, 4}};
  let mut %f_val: <4> f32 = {{1.0, 2.0, 3.0, 4.0}};
  let mut %cond: i1 = 0;
  let mut %cond2: i1 = 0;
  let mut %early_cond: i1 = 0;
  let %one_i32: i32 = 1;
  let %two_i32: i32 = 2;
  let %three_i32: i32 = 3;
  let %four_i32: i32 = 4;
  let %zero_i32: i32 = 0;
  let %one_f32: f32 = 1.0;
  let %two_f32: f32 = 2.0;
  let %three_f32: f32 = 3.0;
  let %four_f32: f32 = 4.0;
  let %zero_f32: f32 = 0.0;
  {lets_str}

^entry:
  {b_entry}
  br ^loop_cond;

^loop_cond:
  {b_loop_cond}
  br %cond != 0, ^body1, ^exit;

^body1:
  {b_body1}
  br ^body2;

^body2:
  {b_body2}
  br %early_cond != 0, ^early_exit, ^latch;

^latch:
  {b_latch}
  br ^loop_cond;

^early_exit:
  {b_early_exit}
  ret 0;

^exit:
  {b_exit}
  ret 0;
}}
"""


# Topology G: Complex Interlocking Blocks (16 Blocks)
def make_topology_g(scenario, is_fp=False, header=""):
  locals_str = scenario.get("locals", "")
  is_fp_scen = is_fp or "f32" in locals_str or "f64" in locals_str
  syms_str, lets_str = partition_locals(locals_str)

  b_entry = backfill_block(scenario.get("entry", ""), is_fp_scen)
  b_pre_left = backfill_block(scenario.get("pre_left", ""), is_fp_scen)
  b_pre_right = backfill_block(scenario.get("pre_right", ""), is_fp_scen)
  b_pre_merge = backfill_block(scenario.get("pre_merge", ""), is_fp_scen)
  b_loop_cond = backfill_block(scenario.get("loop_cond", ""), is_fp_scen)
  b_body1 = backfill_block(scenario.get("body1", ""), is_fp_scen)
  b_body2 = backfill_block(scenario.get("body2", ""), is_fp_scen)
  b_latch = backfill_block(scenario.get("latch", ""), is_fp_scen)
  b_post_split = backfill_block(scenario.get("post_split", ""), is_fp_scen)
  b_post_left = backfill_block(scenario.get("post_left", ""), is_fp_scen)
  b_post_right = backfill_block(scenario.get("post_right", ""), is_fp_scen)
  b_exit = backfill_block(scenario.get("exit", ""), is_fp_scen)

  return f"""{header}
fun @main() : i32 {{
  {syms_str}
  let mut %loop_idx: i32 = 0;
  let %two: i32 = 2;
  let mut %v_val: <4> i32 = {{1, 2, 3, 4}};
  let mut %f_val: <4> f32 = {{1.0, 2.0, 3.0, 4.0}};
  let mut %cond: i1 = 0;
  let mut %cond2: i1 = 0;
  let mut %pre_cond: i1 = 0;
  let mut %post_cond: i1 = 0;
  let %one_i32: i32 = 1;
  let %two_i32: i32 = 2;
  let %three_i32: i32 = 3;
  let %four_i32: i32 = 4;
  let %zero_i32: i32 = 0;
  let %one_f32: f32 = 1.0;
  let %two_f32: f32 = 2.0;
  let %three_f32: f32 = 3.0;
  let %four_f32: f32 = 4.0;
  let %zero_f32: f32 = 0.0;
  {lets_str}

^entry:
  {b_entry}
  br %pre_cond != 0, ^pre_left, ^pre_right;

^pre_left:
  {b_pre_left}
  br ^pre_merge;

^pre_right:
  {b_pre_right}
  br ^pre_merge;

^pre_merge:
  {b_pre_merge}
  br ^loop_cond;

^loop_cond:
  {b_loop_cond}
  br %cond != 0, ^body1, ^post_split;

^body1:
  {b_body1}
  br ^body2;

^body2:
  {b_body2}
  br ^latch;

^latch:
  {b_latch}
  br ^loop_cond;

^post_split:
  {b_post_split}
  br %post_cond != 0, ^post_left, ^post_right;

^post_left:
  {b_post_left}
  br ^exit;

^post_right:
  {b_post_right}
  br ^exit;

^exit:
  {b_exit}
  ret 0;
}}
"""


# Map generators by Topology name
TOPOLOGY_MAP = {
  "A": make_topology_a,
  "B": make_topology_b,
  "C": make_topology_c,
  "D": make_topology_d,
  "E": make_topology_e,
  "F": make_topology_f,
  "G": make_topology_g,
}

# The 16 non-trivial scenarios, mixing vector SIMD with pointer operations and explicit casts
SCENARIOS = {
  # 1. Vector Prefix Scan walking with Pointer (Topology A)
  1: {
    "topology": "A",
    "is_fp": False,
    "locals": "let mut %arr: [8] i32 = {1, 2, 3, 4, 5, 6, 7, 8}; let mut %p: ptr i32 = null; let mut %accum: <4> i32 = {0, 0, 0, 0}; let %one_v: <4> i32 = {1, 1, 1, 1}; let mut %val1: i32 = 0; let mut %val2: i32 = 0; let mut %val3: i32 = 0;",
    "entry": "%p = addr %arr[0];\n  %accum = %v_val;\n  %v_val[0] = 2;",
    "loop_cond": "%cond = cmp < %loop_idx, %two;\n  %accum = %accum + %v_val;\n  %v_val = 1 * %accum;",
    "body1": "%val1 = load %p;\n  %p = %p + %one_i32;\n  %accum[1] = %accum[1] + %val1;",
    "body2": "%val2 = load %p;\n  %p = %p + %one_i32;\n  %accum[2] = %accum[2] + %val2;",
    "body3": "%val3 = load %p;\n  %p = %p + %one_i32;\n  %accum[3] = %accum[3] + %val3;",
    "body4": "%v_val = %accum;\n  %accum = %one_v + %v_val;\n  %v_val = 1 * %accum;",
    "latch": "%loop_idx = %loop_idx + 1;\n  %accum[0] = %accum[0] + 1;\n  %v_val = 1 * %accum;",
    "exit": "require %accum[0] > 0;\n  require %accum[1] > 0;\n  %v_val = 1 * %accum;",
    "post1": "%accum[2] = 22;\n  %accum[3] = 33;\n  %v_val = 1 * %accum;",
    "post2": "%v_val = %accum;\n  %v_val[0] = 5;\n  %v_val = 1 * %v_val;",
    "post3": "require %v_val[0] == 5;\n  require %v_val[2] == 22;\n  %v_val = 1 * %v_val;",
  },
  # 2. Vector Horner's Polynomial Evaluation with Casts (Topology B)
  2: {
    "topology": "B",
    "is_fp": True,
    "locals": "let mut %v_int: <4> i32 = {1, 2, 3, 4}; let mut %x: <4> f32 = {0.0, 0.0, 0.0, 0.0}; let %c2: <4> f32 = {2.0, 2.0, 2.0, 2.0}; let %c1: <4> f32 = {1.5, 1.5, 1.5, 1.5}; let mut %res: <4> f32 = {0.0, 0.0, 0.0, 0.0}; let mut %tmp_i: <4> i32 = {0, 0, 0, 0}; let mut %tmp_f: f32 = 0.0;",
    "entry": "%x = %v_int as <4> f32;\n  %res = %c2 * %x;\n  %f_val = 1.0 * %res;",
    "pre1": "%res = %res + %c1;\n  %tmp_i = %res as <4> i32;\n  %f_val = 1.0 * %res;",
    "pre2": "%res = %res * %x;\n  %res = %tmp_i as <4> f32;\n  %f_val = 1.0 * %res;",
    "pre3": "%res = %res + %c1;\n  %tmp_f = %loop_idx as f32;\n  %f_val = 1.0 * %res;",
    "pre4": "%res = %res * %x;\n  %f_val = %res;\n  %f_val = 1.0 * %res;",
    "pre5": "%res = %res + 5.0;\n  %f_val[0] = %tmp_f;\n  %f_val = 1.0 * %res;",
    "pre6": "%f_val = %res;\n  %f_val[1] = 6.0;\n  %f_val = 1.0 * %res;",
    "loop_cond": "%cond = cmp < %loop_idx, %two;\n  %res = %res + 1.0;\n  %f_val = 1.0 * %res;",
    "body1": "%res = 2.0 * %res;\n  %f_val[2] = 7.0;\n  %f_val = 1.0 * %res;",
    "body2": "%res = %res - 1.0;\n  %f_val[3] = 8.0;\n  %f_val = 1.0 * %res;",
    "body3": "%res = %res + 3.0;\n  %f_val = %res;\n  %f_val = 1.0 * %res;",
    "latch": "%loop_idx = %loop_idx + 1;\n  %res = %res + 1.0;\n  %f_val = 1.0 * %res;",
    "exit": "require %res[0] > 0.0;\n  require %res[1] > 0.0;",
  },
  # 3. Vector Normalization & Pointer Storing (Topology C)
  3: {
    "topology": "C",
    "is_fp": True,
    "locals": "let mut %coords: <4> f32 = {3.0, 4.0, 0.0, 0.0}; let mut %out_arr: [8] f32 = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}; let mut %p_out: ptr f32 = null; let mut %len: <4> f32 = {5.0, 5.0, 5.0, 5.0}; let mut %val1: f32 = 0.0; let mut %val2: f32 = 0.0; let mut %val3: f32 = 0.0;",
    "entry": "%p_out = addr %out_arr[0];\n  %f_val = 1.0 * %len;\n  %f_val[0] = 1.0;",
    "pre1": "%val1 = %coords[0];\n  store %p_out, %val1;\n  %p_out = %p_out + %one_i32;",
    "pre2": "%branch_cond = cmp < %one_f32, %two_f32;\n  %f_val[2] = 3.0;\n  %f_val = 1.0 * %len;",
    "loop_cond": "%cond = cmp < %loop_idx, %two;\n  %len = %len + 1.0;\n  %f_val = 1.0 * %len;",
    "body_start": "%len = %len - 1.0;\n  %f_val[3] = 4.0;\n  %f_val = 1.0 * %len;",
    "body_then": "%val2 = %coords[1];\n  store %p_out, %val2;\n  %p_out = %p_out + %one_i32;",
    "body_else": "%val3 = %coords[2];\n  store %p_out, %val3;\n  %p_out = %p_out + %one_i32;",
    "body_merge": "%len = 1.0 * %len;\n  %f_val[2] = 7.0;\n  %f_val = 1.0 * %len;",
    "latch": "%loop_idx = %loop_idx + 1;\n  %len = %len + 0.1;\n  %f_val = 1.0 * %len;",
    "exit": "require %len[0] > 0.0;\n  require %len[1] > 0.0;",
    "post1": "%f_val = %len;\n  %f_val[3] = 9.0;\n  %f_val = 1.0 * %len;",
  },
  # 4. Vector Standard Deviation with Casts (Topology D)
  4: {
    "topology": "D",
    "is_fp": True,
    "locals": "let mut %sum_v: <4> i32 = {1, 2, 3, 4}; let mut %sum_f: <4> f32 = {0.0, 0.0, 0.0, 0.0}; let mut %mean: <4> f32 = {0.0, 0.0, 0.0, 0.0}; let %four_f: <4> f32 = {4.0, 4.0, 4.0, 4.0};",
    "entry": "%sum_f = %sum_v as <4> f32;\n  %mean = %sum_f / %four_f;\n  %f_val = 1.0 * %mean;",
    "outer_cond": "%cond = cmp < %loop_idx, %two;\n  %inner_cond_val = cmp < %inner_idx, %two;\n  %f_val = 1.0 * %mean;",
    "outer_body1": "%mean = %mean + 1.0;\n  %f_val[1] = 2.0;\n  %f_val = 1.0 * %mean;",
    "inner_cond": "%inner_cond_val = cmp < %inner_idx, %two;\n  %f_val[2] = 3.0;\n  %f_val = 1.0 * %mean;",
    "inner_body1": "%sum_f = %sum_f + 2.0;\n  %f_val[3] = 4.0;\n  %f_val = 1.0 * %mean;",
    "inner_body2": "%mean = %mean - 0.5;\n  %f_val[0] = 5.0;\n  %f_val = 1.0 * %mean;",
    "inner_latch": "%inner_idx = %inner_idx + %one_i32;\n  %f_val[1] = 6.0;\n  %f_val = 1.0 * %mean;",
    "outer_latch": "%loop_idx = %loop_idx + %one_i32;\n  %inner_idx = 0;\n  %f_val = 1.0 * %mean;",
    "exit": "require %mean[0] > 0.0;\n  require %sum_f[0] > 0.0;",
    "post1": "%f_val = %mean;\n  %f_val[2] = 7.0;\n  %f_val = 1.0 * %mean;",
    "post2": "%f_val[3] = 8.0;\n  %f_val = %sum_f;\n  %f_val = 1.0 * %mean;",
    "post3": "require %f_val[0] > 0.0;\n  require %f_val[1] > 0.0;",
  },
  # 5. Vector Mandelbrot with Complex Struct Pointer (Topology E)
  5: {
    "topology": "E",
    "is_fp": True,
    "structs": "struct @Complex { r: f32; i: f32; }\n",
    "locals": "let mut %c_val: @Complex = { -0.7, 0.27 }; let mut %p_r: ptr f32 = null; let mut %p_i: ptr f32 = null; let mut %zr: <4> f32 = {0.0, 0.0, 0.0, 0.0}; let mut %zi: <4> f32 = {0.0, 0.0, 0.0, 0.0}; let mut %zr2: <4> f32 = {0.0, 0.0, 0.0, 0.0}; let mut %zi2: <4> f32 = {0.0, 0.0, 0.0, 0.0}; let %two_f: <4> f32 = {2.0, 2.0, 2.0, 2.0}; let mut %r_val: f32 = 0.0; let mut %i_val: f32 = 0.0;",
    "entry": "%p_r = addr %c_val.r;\n  %p_i = addr %c_val.i;\n  %zr = %zr + 0.1;",
    "l1_cond": "%cond = cmp < %loop_idx, %two;\n  %cond2 = cmp < %loop_idx2, %two;\n  %f_val = 1.0 * %zr;",
    "l1_body": "%r_val = load %p_r;\n  %i_val = load %p_i;\n  %zr[0] = %r_val; %zi[0] = %i_val; %f_val = 1.0 * %zr;",
    "l1_latch": "%loop_idx = %loop_idx + %one_i32;\n  %f_val[1] = 2.0;\n  %f_val = 1.0 * %zr;",
    "mid1": "%f_val = %zr;\n  %f_val[2] = 3.0;\n  %f_val = 1.0 * %zr;",
    "mid2": "%f_val = %zi;\n  %f_val[3] = 4.0;\n  %f_val = 1.0 * %zr;",
    "l2_cond": "%cond2 = cmp < %loop_idx2, %two;\n  %f_val[0] = 5.0;\n  %f_val = 1.0 * %zr;",
    "l2_body": "%zr2 = %zr * %zr;\n  %zi2 = %zi * %zi;\n  %zr = %zr2 - %zi2;",
    "l2_latch": "%loop_idx2 = %loop_idx2 + %one_i32;\n  %f_val[1] = 6.0;\n  %f_val = 1.0 * %zr;",
    "exit": "require %zr[0] > -10.0;\n  require %zi[0] < 2.0;",
    "post1": "%f_val = %zr;\n  %f_val[2] = 7.0;\n  %f_val = 1.0 * %zr;",
  },
  # 6. Coordinate Rotation using Pointers (Topology F)
  6: {
    "topology": "F",
    "is_fp": True,
    "structs": "struct @Vector2D { x: f32; y: f32; }\n",
    "locals": "let mut %coords: <4> f32 = {1.0, 0.0, 0.0, 1.0}; let mut %out_arr: [8] f32 = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}; let mut %p_x: ptr f32 = null; let %cos_val: <4> f32 = {0.866, 0.866, 0.866, 0.866}; let %sin_val: <4> f32 = {0.5, 0.5, 0.5, 0.5}; let mut %x_new: <4> f32 = 0.0; let mut %rot_x: f32 = 0.0;",
    "entry": "%p_x = addr %out_arr[0];\n  %x_new = %coords * %cos_val;",
    "loop_cond": "%cond = cmp < %loop_idx, %two;\n  %early_cond = cmp < %one_f32, %zero_f32;\n  %f_val = 1.0 * %x_new;",
    "body1": "%x_new = %coords * %cos_val - %sin_val;\n  %coords = %x_new;\n  %f_val = 1.0 * %x_new;",
    "body2": "%rot_x = %coords[0];\n  store %p_x, %rot_x;\n  %p_x = %p_x + %one_i32;",
    "latch": "%loop_idx = %loop_idx + 1;\n  %v_val[2] = 3;\n  %f_val = 1.0 * %x_new;",
    "early_exit": "%f_val = %coords;\n  %f_val[3] = 4.0;\n  %f_val = 1.0 * %x_new;",
    "exit": "require %coords[0] < 2.0;\n  require %coords[1] < 2.0;",
  },
  # 7. Linear Interpolation with Cast (Topology G)
  7: {
    "topology": "G",
    "is_fp": True,
    "locals": "let mut %v_int: <4> i32 = {10, 20, 30, 40}; let mut %v_float: <4> f32 = {0.0, 0.0, 0.0, 0.0}; let %b: <4> f32 = {100.0, 200.0, 300.0, 400.0}; let %t: <4> f32 = {0.5, 0.5, 0.5, 0.5}; let mut %res: <4> f32 = 0.0; let mut %arr: [2] f32 = {1.0, 2.0}; let mut %p1: ptr f32 = null; let mut %p2: ptr f32 = null; let mut %p_sel: ptr f32 = null;",
    "entry": "%pre_cond = cmp < %one_i32, %two_i32;\n  %post_cond = cmp < %two_i32, %one_i32;\n  %p1 = addr %arr[0]; %p2 = addr %arr[1];",
    "pre_left": "%v_float = %v_int as <4> f32;\n  %res = %b - %v_float;\n  %f_val = 1.0 * %res;",
    "pre_right": "%res = %v_float - %b;\n  %f_val[1] = 2.0;\n  %f_val = 1.0 * %res;",
    "pre_merge": "%p_sel = select %pre_cond, %p1, %p2;\n  %f_val = 1.0 * %res;\n  %f_val = 1.0 * %res;",
    "loop_cond": "%cond = cmp < %loop_idx, %two;\n  %res = 1.0 * %res;\n  %f_val = 1.0 * %res;",
    "body1": "%res = %v_float + %t * %res;\n  %f_val[3] = 4.0;\n  %f_val = 1.0 * %res;",
    "body2": "%f_val = %res;\n  %f_val[0] = 5.0;\n  %f_val = 1.0 * %res;",
    "latch": "%loop_idx = %loop_idx + 1;\n  %f_val[1] = 6.0;\n  %f_val = 1.0 * %res;",
    "post_split": "%f_val = %res;\n  %f_val[2] = 7.0;\n  %f_val = 1.0 * %res;",
    "post_left": "%f_val[3] = 8.0;\n  %f_val = %res;\n  %f_val = 1.0 * %res;",
    "post_right": "%f_val[3] = 9.0;\n  %f_val = %res;\n  %f_val = 1.0 * %res;",
    "exit": "require %res[0] > 0.0;\n  require %res[1] > 0.0;",
  },
  # 8. Threshold Binarization using Pointers (Topology A)
  8: {
    "topology": "A",
    "is_fp": False,
    "locals": "let mut %arr: [8] i32 = {15, 5, 5, 15, 35, 5, 0, 0}; let mut %p: ptr i32 = null; let %p_null: ptr i32 = null; let mut %v: <4> i32 = {0, 0, 0, 0}; let %threshold: <4> i32 = {20, 20, 20, 20}; let %ones: <4> i32 = {1, 1, 1, 1}; let %zeros: <4> i32 = {0, 0, 0, 0}; let mut %mask: <4> i1 = {0, 0, 0, 0}; let mut %bin: <4> i32 = 0; let mut %val1: i32 = 0; let mut %val2: i32 = 0; let mut %val3: i32 = 0; let mut %ptr_cond: i1 = 0;",
    "entry": "%p = addr %arr[0];\n  %ptr_cond = cmp != %p, %p_null;\n  %mask = cmp > %v, %threshold;",
    "loop_cond": "%cond = cmp < %loop_idx, %two;\n  %bin = select %mask, %ones, %zeros;\n  %v_val = 1 * %bin;",
    "body1": "%val1 = load %p;\n  %v[0] = %val1;\n  %p = %p + %one_i32;",
    "body2": "%val2 = load %p;\n  %v[1] = %val2;\n  %p = %p + %one_i32;",
    "body3": "%val3 = load %p;\n  %v[2] = %val3;\n  %p = %p + %one_i32;",
    "body4": "%mask = cmp > %v, %threshold;\n  %bin = select %mask, %ones, %zeros;\n  %v_val = 1 * %bin;",
    "latch": "%loop_idx = %loop_idx + 1;\n  %v_val[0] = 5;\n  %v_val = 1 * %bin;",
    "exit": "require %bin[0] == 0;\n  require %bin[1] == 1;\n  %v_val = 1 * %bin;",
    "post1": "%v_val[1] = 6;\n  %v_val = %bin;\n  %v_val = 1 * %v_val;",
    "post2": "%v_val[2] = 7;\n  %v_val = %ones;\n  %v_val = 1 * %v_val;",
    "post3": "require %v_val[0] == 1;\n  require %v_val[1] == 1;\n  %v_val = 1 * %v_val;",
  },
  # 9. Dot Product with aggregate array pointers and subtraction (Topology B)
  9: {
    "topology": "B",
    "is_fp": False,
    "locals": "let mut %arr1: [8] i32 = {1, 2, 3, 4, 0, 0, 0, 0}; let mut %arr2: [8] i32 = {5, 6, 7, 8, 0, 0, 0, 0}; let mut %p1: ptr i32 = null; let mut %p2: ptr i32 = null; let mut %dot: i32 = 0; let mut %v1: i32 = 0; let mut %v2: i32 = 0; let mut %v3: i32 = 0; let mut %v4: i32 = 0; let mut %v5: i32 = 0; let mut %v6: i32 = 0; let mut %v7: i32 = 0; let mut %v8: i32 = 0; let mut %p1_end: ptr i32 = null; let mut %dist: i64 = 0;",
    "entry": "%p1 = addr %arr1[0];\n  %p2 = addr %arr2[0];\n  %p1_end = addr %arr1[7];",
    "pre1": "%v1 = load %p1;\n  %v2 = load %p2;\n  %dot = %v1 * %v2;",
    "pre2": "%p1 = %p1 + %one_i32;\n  %p2 = %p2 + %one_i32;\n  %dist = %p1_end - %p1;",
    "pre3": "%v3 = load %p1;\n  %v4 = load %p2;\n  %dot = %dot + %v3 * %v4;",
    "pre4": "%p1 = %p1 + %one_i32;\n  %p2 = %p2 + %one_i32;\n  %v_val = 1 * %v_val;",
    "pre5": "%v5 = load %p1;\n  %v6 = load %p2;\n  %dot = %dot + %v5 * %v6;",
    "pre6": "%p1 = %p1 + %one_i32;\n  %p2 = %p2 + %one_i32;\n  %v_val = 1 * %v_val;",
    "loop_cond": "%cond = cmp < %loop_idx, %two;\n  %dot = %dot + 1;\n  %v_val = 1 * %v_val;",
    "body1": "%v7 = load %p1;\n  %v8 = load %p2;\n  %dot = %dot + %v7 * %v8;",
    "body2": "%p1 = %p1 + %one_i32;\n  %p2 = %p2 + %one_i32;\n  %v_val = 1 * %v_val;",
    "body3": "%v_val[0] = %dot;\n  %v_val[1] = %dot;\n  %v_val = 1 * %v_val;",
    "latch": "%loop_idx = %loop_idx + 1;\n  %dot = %dot + 1;\n  %v_val = 1 * %v_val;",
    "exit": "require %dot > 0;\n  require %v_val[0] > 0;\n  require %dist > 0;",
  },
  # 10. Matrix Multiplication step with Casts (Topology C)
  10: {
    "topology": "C",
    "is_fp": True,
    "locals": "let mut %v_int: <4> i32 = {2, 3, 0, 0}; let mut %v_float: <4> f32 = {0.0, 0.0, 0.0, 0.0}; let %row0: <4> f32 = {1.5, 2.5, 0.0, 0.0}; let %row1: <4> f32 = {3.5, 4.5, 0.0, 0.0}; let mut %res: <4> f32 = {0.0, 0.0, 0.0, 0.0}; let mut %r0_val: f32 = 0.0; let mut %r1_val: f32 = 0.0; let mut %scalar_cast: f32 = 0.0;",
    "entry": "%v_float = %v_int as <4> f32;\n  %r0_val = %row0[0];\n  %res[0] = %r0_val * %v_float[0];\n  %f_val = 1.0 * %res;",
    "pre1": "%r1_val = %row1[0];\n  %res[1] = %r1_val * %v_float[0];\n  %scalar_cast = %loop_idx as f32;",
    "pre2": "%branch_cond = cmp < %res[0], %res[1];\n  %f_val[2] = 3.0;\n  %f_val = 1.0 * %res;",
    "loop_cond": "%cond = cmp < %loop_idx, %two;\n  %res[0] = %res[0] + %scalar_cast;\n  %f_val = 1.0 * %res;",
    "body_start": "%res[1] = %res[1] + 1.0;\n  %f_val[3] = 4.0;\n  %f_val = 1.0 * %res;",
    "body_then": "%res[0] = %res[0] + 5.0;\n  %f_val[0] = 5.0;\n  %f_val = 1.0 * %res;",
    "body_else": "%res[1] = %res[1] + 5.0;\n  %f_val[1] = 6.0;\n  %f_val = 1.0 * %res;",
    "body_merge": "%res[0] = 1.0 * %res[0];\n  %f_val[2] = 7.0;\n  %f_val = 1.0 * %res;",
    "latch": "%loop_idx = %loop_idx + 1;\n  %res[1] = 1.0 * %res[1];\n  %f_val = 1.0 * %res;",
    "exit": "require %res[0] > 0.0;\n  require %res[1] > 0.0;",
    "post1": "%f_val = %res;\n  %f_val[3] = 9.0;\n  %f_val = 1.0 * %res;",
  },
  # 11. Bitwise Hashing and Pointer store (Topology D)
  11: {
    "topology": "D",
    "is_fp": False,
    "locals": "let mut %hash_table: [8] i32 = {0, 0, 0, 0, 0, 0, 0, 0}; let mut %p: ptr i32 = null; let mut %keys: <4> i32 = {111, 222, 333, 444}; let %mask_v: <4> i32 = {127, 127, 127, 127}; let mut %hash: <4> i32 = 0; let mut %h0: i32 = 0;",
    "entry": "%p = addr %hash_table[0];\n  %hash = %keys ^ %mask_v;\n  %v_val = 1 * %hash;",
    "outer_cond": "%cond = cmp < %loop_idx, %two;\n  %inner_cond_val = cmp < %inner_idx, %two;\n  %v_val = 1 * %hash;",
    "outer_body1": "%hash = %hash & %mask_v;\n  %v_val[1] = 2;\n  %v_val = 1 * %hash;",
    "inner_cond": "%inner_cond_val = cmp < %inner_idx, %two;\n  %v_val[2] = 3;\n  %v_val = 1 * %hash;",
    "inner_body1": "%hash = ~ %hash;\n  %v_val[3] = 4;\n  %v_val = 1 * %hash;",
    "inner_body2": "%h0 = %hash[0];\n  store %p, %h0;\n  %p = %p + %one_i32;",
    "inner_latch": "%inner_idx = %inner_idx + %one_i32;\n  %v_val[1] = 6;\n  %v_val = 1 * %hash;",
    "outer_latch": "%loop_idx = %loop_idx + %one_i32;\n  %inner_idx = 0;\n  %v_val = 1 * %hash;",
    "exit": "require %hash_table[0] != 0;\n  require %hash[0] != 0;\n  %v_val = 1 * %hash;",
    "post1": "%v_val = %hash;\n  %v_val[2] = 7;\n  %v_val = 1 * %hash;",
    "post2": "%v_val[3] = 8;\n  %v_val = %mask_v;\n  %v_val[0] = %mask_v[0];",
    "post3": "require %v_val[0] == 127;\n  require %v_val[1] == 127;\n  %v_val = 1 * %hash;",
  },
  # 12. Range Clamp with float casts and pointer (Topology E)
  12: {
    "topology": "E",
    "is_fp": True,
    "locals": "let mut %v_int: <4> i32 = {-10, 150, 50, 120}; let mut %v_float: <4> f32 = {0.0, 0.0, 0.0, 0.0}; let %lo: <4> f32 = {0.0, 0.0, 0.0, 0.0}; let %hi: <4> f32 = {100.0, 100.0, 100.0, 100.0}; let mut %too_low: <4> i1 = {0, 0, 0, 0}; let mut %too_high: <4> i1 = {0, 0, 0, 0}; let mut %back_to_int: <4> i32 = {0, 0, 0, 0};",
    "entry": "%v_float = %v_int as <4> f32;\n  %too_low = cmp < %v_float, %lo;\n  %f_val = 1.0 * %v_float;",
    "l1_cond": "%cond = cmp < %loop_idx, %two;\n  %cond2 = cmp < %loop_idx2, %two;\n  %f_val = 1.0 * %v_float;",
    "l1_body": "%too_high = cmp > %v_float, %hi;\n  %v_float = select %too_low, %lo, %v_float;\n  %v_float = select %too_high, %hi, %v_float;",
    "l1_latch": "%loop_idx = %loop_idx + %one_i32;\n  %f_val[1] = 2.0;\n  %f_val = 1.0 * %v_float;",
    "mid1": "%back_to_int = %v_float as <4> i32;\n  %f_val[2] = 3.0;\n  %f_val = 1.0 * %v_float;",
    "mid2": "%f_val = %v_float;\n  %f_val[3] = 4.0;\n  %f_val = 1.0 * %v_float;",
    "l2_cond": "%cond2 = cmp < %loop_idx2, %two;\n  %f_val[0] = 5.0;\n  %f_val = 1.0 * %v_float;",
    "l2_body": "%too_low = cmp < %v_float, %lo;\n  %too_high = cmp > %v_float, %hi;\n  %v_float = select %too_low, %lo, %v_float;\n  %v_float = select %too_high, %hi, %v_float;",
    "l2_latch": "%loop_idx2 = %loop_idx2 + %one_i32;\n  %f_val[1] = 6.0;\n  %f_val = 1.0 * %v_float;",
    "exit": "require %v_float[0] == 0.0;\n  require %v_float[1] == 100.0;",
    "post1": "%f_val = %v_float;\n  %f_val[2] = 7.0;\n  %f_val = 1.0 * %v_float;",
  },
  # 13. Sorting Network Step with aggregate array pointers (Topology F)
  13: {
    "topology": "F",
    "is_fp": False,
    "locals": "let mut %arr: [8] i32 = {10, 5, 20, 15, 0, 0, 0, 0}; let mut %p: ptr i32 = null; let mut %v: <4> i32 = {0, 0, 0, 0}; let mut %tmp: i32 = 0; let mut %cflag: i1 = 0; let mut %v0: i32 = 0; let mut %v1: i32 = 0;",
    "entry": "%p = addr %arr[0];\n  %v_val = 1 * %v;\n  %v_val[0] = 1;",
    "loop_cond": "%cond = cmp < %loop_idx, %two;\n  %early_cond = cmp < %one_i32, %zero_i32;\n  %v_val = 1 * %v;",
    "body1": "%v0 = load %p;\n  %v[0] = %v0;\n  %p = %p + %one_i32;",
    "body2": "%v1 = load %p;\n  %v[1] = %v1;\n  %cflag = cmp > %v[0], %v[1];",
    "latch": "%loop_idx = %loop_idx + 1;\n  %v_val[2] = 3;\n  %v_val = 1 * %v;",
    "early_exit": "%f_val[0] = 1.0;\n  %v_val[3] = 4;\n  %v_val = 1 * %v;",
    "exit": "require %v[0] > 0;\n  require %v[1] > 0;",
  },
  # 14. Fibonacci with aggregate array pointers and subtraction (Topology G)
  14: {
    "topology": "G",
    "is_fp": False,
    "locals": "let mut %arr: [8] i32 = {0, 0, 0, 0, 0, 0, 0, 0}; let mut %p_start: ptr i32 = null; let mut %p_end: ptr i32 = null; let mut %dist: i64 = 0; let mut %fib: <4> i32 = {0, 1, 1, 2};",
    "entry": "%pre_cond = cmp < %one_i32, %two_i32;\n  %post_cond = cmp < %two_i32, %one_i32;\n  %p_start = addr %arr[0];\n  %p_end = addr %arr[3];\n  %v_val = 1 * %fib;",
    "pre_left": "%dist = %p_end - %p_start;\n  %v_val[0] = 1;\n  %v_val = 1 * %fib;",
    "pre_right": "%dist = %p_start - %p_end;\n  %v_val[1] = 2;\n  %v_val = 1 * %fib;",
    "pre_merge": "%v_val = %fib;\n  %v_val[2] = 3;\n  %v_val = 1 * %fib;",
    "loop_cond": "%cond = cmp < %loop_idx, %two;\n  %fib[0] = %fib[1];\n  %v_val = 1 * %fib;",
    "body1": "%fib[1] = %fib[2];\n  %v_val[3] = 4;\n  %v_val = 1 * %fib;",
    "body2": "%v_val = %fib;\n  %v_val[0] = 5;\n  %v_val = 1 * %fib;",
    "latch": "%loop_idx = %loop_idx + 1;\n  %v_val[1] = 6;\n  %v_val = 1 * %fib;",
    "post_split": "%v_val = %fib;\n  %v_val[2] = 7;\n  %v_val = 1 * %fib;",
    "post_left": "%v_val[3] = 8;\n  %v_val = %fib;\n  %v_val = 1 * %fib;",
    "post_right": "%v_val[3] = 9;\n  %v_val = %fib;\n  %v_val = 1 * %fib;",
    "exit": "require %dist == 3;\n  require %fib[0] > 0;\n  %v_val = 1 * %fib;",
  },
  # 15. Weighted Moving Average with Casts (Topology A)
  15: {
    "topology": "A",
    "is_fp": True,
    "locals": "let mut %w_int: <4> i32 = {1, 2, 3, 4}; let mut %w_float: <4> f32 = {0.0, 0.0, 0.0, 0.0}; let %v: <4> f32 = {10.0, 20.0, 30.0, 40.0}; let mut %res: <4> f32 = {0.0, 0.0, 0.0, 0.0}; let mut %back_int: <4> i32 = {0, 0, 0, 0};",
    "entry": "%w_float = %w_int as <4> f32;\n  %res = %v * %w_float;\n  %f_val = 1.0 * %res;",
    "loop_cond": "%cond = cmp < %loop_idx, %two;\n  %w_float = %w_float + 1.0;\n  %f_val = 1.0 * %res;",
    "body1": "%res = %v * %w_float + %res;\n  %f_val[1] = 2.0;\n  %f_val = 1.0 * %res;",
    "body2": "%res = 0.5 * %res;\n  %f_val[2] = 3.0;\n  %f_val = 1.0 * %res;",
    "body3": "%res = %res - 0.5;\n  %back_int = %res as <4> i32;\n  %f_val = 1.0 * %res;",
    "body4": "%res = %v * %w_float - %res;\n  %f_val = %res;\n  %f_val = 1.0 * %res;",
    "latch": "%loop_idx = %loop_idx + 1;\n  %f_val[0] = 5.0;\n  %f_val = 1.0 * %res;",
    "exit": "require %res[0] > -10.0;\n  require %res[1] > -10.0;\n  %f_val = 1.0 * %res;",
    "post1": "%f_val[1] = 6.0;\n  %f_val = %res;\n  %f_val = 1.0 * %res;",
    "post2": "%f_val[2] = 7.0;\n  %f_val = %w_float;\n  %f_val = 1.0 * %res;",
    "post3": "require %f_val[0] > 0.0;\n  require %f_val[1] > 0.0;\n  %f_val = 1.0 * %res;",
  },
  # 16. Exponential Decay with Pointer Stepping (Topology B)
  16: {
    "topology": "B",
    "is_fp": True,
    "locals": "let mut %arr: [8] f32 = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}; let mut %p: ptr f32 = null; let mut %v: <4> f32 = {100.0, 100.0, 100.0, 100.0}; let %decay: <4> f32 = {0.9, 0.9, 0.9, 0.9}; let mut %steps: i32 = 0; let mut %val1: f32 = 0.0; let mut %val2: f32 = 0.0;",
    "entry": "%p = addr %arr[0];\n  %v = %v * %decay;\n  %f_val = 1.0 * %v;",
    "pre1": "%steps = %steps + 1;\n  %f_val[1] = 2.0;\n  %f_val = 1.0 * %v;",
    "pre2": "%v = %v * %decay;\n  %f_val[2] = 3.0;\n  %f_val = 1.0 * %v;",
    "pre3": "%steps = %steps + 1;\n  %f_val[3] = 4.0;\n  %f_val = 1.0 * %v;",
    "pre4": "%v = %v * %decay;\n  %f_val = %v;\n  %f_val = 1.0 * %v;",
    "pre5": "%steps = %steps + 1;\n  %f_val[0] = 5.0;\n  %f_val = 1.0 * %v;",
    "pre6": "%f_val = %v;\n  %f_val[1] = 6.0;\n  %f_val = 1.0 * %v;",
    "loop_cond": "%cond = cmp < %loop_idx, %two;\n  %v = %v * %decay;\n  %f_val = 1.0 * %v;",
    "body1": "%val1 = %v[0];\n  store %p, %val1;\n  %p = %p + %one_i32;",
    "body2": "%val2 = %v[1];\n  store %p, %val2;\n  %p = %p + %one_i32;",
    "body3": "%f_val = %v;\n  %f_val = 1.0 * %v;\n  %f_val = 1.0 * %v;",
    "latch": "%loop_idx = %loop_idx + 1;\n  %steps = %steps + 1;\n  %f_val = 1.0 * %v;",
    "exit": "require %v[0] < 100.0;\n  require %v[1] < 100.0;",
  },
}

VALID_INTENTIONS = {
  1: "Vector Prefix Scan walking with Pointer",
  2: "Vector Horner's Polynomial Evaluation with Casts",
  3: "Vector Normalization & Pointer Storing",
  4: "Vector Standard Deviation with Casts",
  5: "Vector Mandelbrot with Complex Struct Pointer",
  6: "Coordinate Rotation using Pointers",
  7: "Linear Interpolation with Cast",
  8: "Threshold Binarization using Pointers",
  9: "Dot Product with aggregate array pointers and subtraction",
  10: "Matrix Multiplication step with Casts",
  11: "Bitwise Hashing and Pointer store",
  12: "Range Clamp with float casts and pointer",
  13: "Sorting Network Step with aggregate array pointers",
  14: "Fibonacci with aggregate array pointers and subtraction",
  15: "Weighted Moving Average with Casts",
  16: "Exponential Decay with Pointer Stepping",
}

INVALID_INTENTIONS = {
  "lexer": {
    17: "Lexer error: invalid character '$' in basic block body_then",
    18: "Lexer error: malformed symbol/variable sigil '%??x' in basic block inner_body1",
    19: "Lexer error: bad exponent format in float vector literal in basic block l1_body",
    20: "Lexer error: unterminated block comment in basic block body1",
    21: "Lexer error: invalid character '?' in label name '^latch?:'",
  },
  "parser": {
    17: "Parser error: missing comma separator in brace initializer of a vector",
    18: "Parser error: double brackets '[[0]]' in vector lane access",
    19: "Parser error: mismatched vector type syntax (missing '>')",
    20: "Parser error: malformed select expression with missing alternative arm",
    21: "Parser error: malformed cmp expression missing comma between operands",
  },
  "typechecker": {
    17: "Typechecker error: mixing mismatched vector sizes (<4> i32 + <3> i32) in addition",
    18: "Typechecker error: declaring pointer to vector type (ptr <4> i32) which is unsupported",
    19: "Typechecker error: incompatible cast size casting <4> i32 to <3> i32",
    20: "Typechecker accepts bitwise operations on float vectors (exposing known bug, expecting PASS)",
    21: "Typechecker error: mismatched select arm types (vector of i32 vs vector of f32)",
  },
  "semchecker": {
    17: "Semchecker error: mutating an immutable local variable inside a loop",
    18: "Semchecker error: taking address of a vector variable which is not addressable",
    19: "Semchecker error: definite initialization failure when reading an unwritten undef vector lane",
    20: "Semchecker error: struct declaration contains a vector type field",
    21: "Semchecker error: duplicate local variable declaration",
  },
  "interp": {
    17: "Interpreter runtime UB: vector lane subscript index out of bounds",
    18: "Interpreter runtime UB: division/modulo by zero in a vector lane",
    19: "Interpreter runtime UB: signed integer overflow in vector lane addition",
    20: "Interpreter runtime UB: out-of-bounds float-to-int cast in vector lane",
    21: "Interpreter runtime UB: reading an undefined vector lane",
  },
  "compile": {
    17: "Compiler static check: mutating immutable vector local",
    18: "Compiler static check: taking address of a vector",
    19: "Compiler static check: struct with vector field",
    20: "Compiler static check: vector size cast mismatch",
    21: "Compiler static check: mismatched select types",
  },
  "solver": {
    17: "Solver check: contradictory path/require constraints resulting in UNSAT",
    18: "Solver check: path forces vector lane access out of bounds",
    19: "Solver check: path forces division by zero in vector lane",
    20: "Solver check: path forces out-of-range float-to-int cast",
    21: "Solver check: path forces signed integer overflow in vector lane",
  },
}


# Generate valid SIR code by instantiating the right topology
def get_valid_sir(scenario_idx, component, extra_header=""):
  scen = SCENARIOS[scenario_idx]
  topol = scen["topology"]
  is_fp = scen.get("is_fp", False)
  gen_func = TOPOLOGY_MAP[topol]

  structs_str = scen.get("structs", "")
  header = structs_str + extra_header
  if component == "solver":
    # Formulate paths based on Topology
    path_str = ""
    if topol == "A":
      path_str = "^entry,^loop_cond,^body1,^body2,^body3,^body4,^latch,^loop_cond,^body1,^body2,^body3,^body4,^latch,^loop_cond,^exit,^post1,^post2,^post3"
    elif topol == "B":
      path_str = "^entry,^pre1,^pre2,^pre3,^pre4,^pre5,^pre6,^loop_cond,^body1,^body2,^body3,^latch,^loop_cond,^body1,^body2,^body3,^latch,^loop_cond,^exit"
    elif topol == "C":
      path_str = "^entry,^pre1,^pre2,^loop_cond,^body_start,^body_then,^body_merge,^latch,^loop_cond,^body_start,^body_then,^body_merge,^latch,^loop_cond,^exit,^post1"
    elif topol == "D":
      path_str = "^entry,^outer_cond,^outer_body1,^inner_cond,^inner_body1,^inner_body2,^inner_latch,^inner_cond,^inner_body1,^inner_body2,^inner_latch,^inner_cond,^outer_latch,^outer_cond,^exit,^post1,^post2,^post3"
    elif topol == "E":
      path_str = "^entry,^l1_cond,^l1_body,^l1_latch,^l1_cond,^l1_body,^l1_latch,^l1_cond,^mid1,^mid2,^l2_cond,^l2_body,^l2_latch,^l2_cond,^l2_body,^l2_latch,^l2_cond,^exit,^post1"
    elif topol == "F":
      path_str = "^entry,^loop_cond,^body1,^body2,^latch,^loop_cond,^body1,^body2,^latch,^loop_cond,^exit"
    elif topol == "G":
      path_str = "^entry,^pre_left,^pre_merge,^loop_cond,^body1,^body2,^latch,^loop_cond,^body1,^body2,^latch,^loop_cond,^post_split,^post_left,^exit"
    header = f"// SOLVER_ARGS: --main @main --path '{path_str}'\n" + header
  elif component == "compile":
    # Add compiler arguments
    header = "// COMPILER_ARGS:\n" + header

  return gen_func(scen, is_fp, header)


# Write all valid tests (16 per component)
for comp, comp_dir in TEST_DIRS.items():
  for scen_idx in range(1, 17):
    filename = f"{comp_dir}/new_{comp}_test_{scen_idx:02d}.sir"
    intent = VALID_INTENTIONS[scen_idx]
    content = get_valid_sir(
      scen_idx, comp, extra_header=f"// EXPECT: PASS\n// Intention: {intent}\n"
    )
    with open(filename, "w") as f:
      f.write(content)

print("Finished writing 112 valid test cases across 7 components.")

# Define invalid cases for each component
# We map invalid cases to Topologies C, D, E, F, G to keep CFGs varied and non-trivial.
# Every basic block in invalid cases also has at least 3 instructions.

# 1. Lexer Invalid Cases
# Fail at tokenization/lexing
lexer_invalid = [
  # 17: Topology C - Invalid character `$` inside body_then
  {
    "topology": "C",
    "is_fp": False,
    "expect": "FAIL:LexError",
    "scen": {
      "locals": "let mut %x: <4> i32 = 0;",
      "entry": "%x[0] = 1;\n  %x[1] = 2;\n  %x[2] = 3;",
      "pre1": "%x[3] = 4;\n  %v_val[0] = 5;\n  %v_val[1] = 6;",
      "pre2": "%branch_cond = cmp < %one_i32, %two_i32;\n  %v_val[2] = 7;\n  %v_val[3] = 8;",
      "loop_cond": "%cond = cmp < %loop_idx, %two;\n  %v_val[0] = 9;\n  %v_val[1] = 10;",
      "body_start": "%v_val[2] = 11;\n  %v_val[3] = 12;\n  %v_val[0] = 13;",
      "body_then": "%x = %x + $;  // Lexer error: $ is invalid character\n  %v_val[1] = 14;\n  %v_val[2] = 15;",
      "body_else": "%x = %x + %one_i32;\n  %v_val[1] = 16;\n  %v_val[2] = 17;",
      "body_merge": "%x = %x - %one_i32;\n  %v_val[3] = 18;\n  %v_val[0] = 19;",
      "latch": "%loop_idx = %loop_idx + 1;\n  %v_val[1] = 20;\n  %v_val[2] = 21;",
      "exit": "%v_val[3] = 22;\n  %v_val[0] = 23;\n  %v_val[1] = 24;",
      "post1": "%v_val[2] = 25;\n  %v_val[3] = 26;\n  %v_val[0] = 27;",
    },
  },
  # 18: Topology D - Malformed symbol sigil `%??x` inside inner_body1
  {
    "topology": "D",
    "is_fp": False,
    "expect": "FAIL:LexError",
    "scen": {
      "locals": "let mut %x: <4> i32 = 0;",
      "entry": "%x[0] = 1;\n  %x[1] = 2;\n  %x[2] = 3;",
      "outer_cond": "%cond = cmp < %loop_idx, %two;\n  %inner_cond_val = cmp < %inner_idx, %two;\n  %v_val[0] = 4;",
      "outer_body1": "%x[3] = 4;\n  %v_val[1] = 5;\n  %v_val[2] = 6;",
      "inner_cond": "%inner_cond_val = cmp < %inner_idx, %two;\n  %v_val[3] = 7;\n  %v_val[0] = 8;",
      "inner_body1": "%x = %??x;  // Lexer error: malformed sigil\n  %v_val[1] = 9;\n  %v_val[2] = 10;",
      "inner_body2": "%x[1] = 5;\n  %v_val[3] = 11;\n  %v_val[0] = 12;",
      "inner_latch": "%inner_idx = %inner_idx + 1;\n  %v_val[1] = 13;\n  %v_val[2] = 14;",
      "outer_latch": "%loop_idx = %loop_idx + 1;\n  %inner_idx = 0;\n  %v_val[3] = 15;",
      "exit": "%v_val[0] = 16;\n  %v_val[1] = 17;\n  %v_val[2] = 18;",
      "post1": "%v_val[3] = 19;\n  %v_val[0] = 20;\n  %v_val[1] = 21;",
      "post2": "%v_val[2] = 22;\n  %v_val[3] = 23;\n  %v_val[0] = 24;",
      "post3": "%v_val[1] = 25;\n  %v_val[2] = 26;\n  %v_val[3] = 27;",
    },
  },
  # 19: Topology E - Bad exponent in float vector literal inside l1_body
  {
    "topology": "E",
    "is_fp": True,
    "expect": "FAIL:LexError",
    "scen": {
      "locals": "let mut %x: <4> f32 = 0.0;",
      "entry": "%x[0] = 1.0;\n  %f_val[0] = 1.0;\n  %f_val[1] = 2.0;",
      "l1_cond": "%cond = cmp < %loop_idx, %two;\n  %cond2 = cmp < %loop_idx2, %two;\n  %f_val[2] = 3.0;",
      "l1_body": "%x = {1.0e+, 2.0, 3.0, 4.0};  // Lexer error: malformed float exp\n  %f_val[3] = 4.0;\n  %f_val[0] = 5.0;",
      "l1_latch": "%loop_idx = %loop_idx + 1;\n  %f_val[1] = 6.0;\n  %f_val[2] = 7.0;",
      "mid1": "%f_val[3] = 8.0;\n  %f_val[0] = 9.0;\n  %f_val[1] = 10.0;",
      "mid2": "%f_val[2] = 11.0;\n  %f_val[3] = 12.0;\n  %f_val[0] = 13.0;",
      "l2_cond": "%cond2 = cmp < %loop_idx2, %two;\n  %f_val[1] = 14.0;\n  %f_val[2] = 15.0;",
      "l2_body": "%x[1] = 2.0;\n  %f_val[3] = 16.0;\n  %f_val[0] = 17.0;",
      "l2_latch": "%loop_idx2 = %loop_idx2 + 1;\n  %f_val[1] = 18.0;\n  %f_val[2] = 19.0;",
      "exit": "%f_val[3] = 20.0;\n  %f_val[0] = 21.0;\n  %f_val[1] = 22.0;",
      "post1": "%f_val[2] = 23.0;\n  %f_val[3] = 24.0;\n  %f_val[0] = 25.0;",
    },
  },
  # 20: Topology F - Unterminated comment block inside body1
  {
    "topology": "F",
    "is_fp": False,
    "expect": "FAIL:ParseError",
    "scen": {
      "locals": "let mut %x: <4> i32 = 0;",
      "entry": "%x[0] = 1;\n  %v_val[0] = 1;\n  %v_val[1] = 2;",
      "loop_cond": "%cond = cmp < %loop_idx, %two;\n  %early_cond = cmp < %one_i32, %zero_i32;\n  %v_val[2] = 3;",
      "body1": "/* Unterminated block comment\n  %x[1] = 2;\n  %v_val[3] = 4;",
      "body2": "%v_val[0] = 5;\n  %v_val[1] = 6;\n  %v_val[2] = 7;",
      "latch": "%loop_idx = %loop_idx + 1;\n  %v_val[3] = 8;\n  %v_val[0] = 9;",
      "early_exit": "%v_val[1] = 10;\n  %v_val[2] = 11;\n  %v_val[3] = 12;",
      "exit": "%v_val[0] = 13;\n  %v_val[1] = 14;\n  %v_val[2] = 15;",
    },
  },
  # 21: Topology G - Invalid character `?` in basic block label
  {
    "topology": "G",
    "is_fp": False,
    "expect": "FAIL:LexError",
    "scen": {
      "locals": "let mut %x: <4> i32 = 0;",
      "entry": "%pre_cond = cmp < %one_i32, %two_i32;\n  %post_cond = cmp < %two_i32, %one_i32;\n  %v_val[0] = 1;",
      "pre_left": "%x[0] = 1;\n  %v_val[1] = 2;\n  %v_val[2] = 3;",
      "pre_right": "%x[1] = 2;\n  %v_val[3] = 4;\n  %v_val[0] = 5;",
      "pre_merge": "%v_val[1] = 6;\n  %v_val[2] = 7;\n  %v_val[3] = 8;",
      "loop_cond": "%cond = cmp < %loop_idx, %two;\n  %v_val[0] = 9;\n  %v_val[1] = 10;",
      "body1": "%x[2] = 3;\n  %v_val[2] = 11;\n  %v_val[3] = 12;",
      "body2": "%v_val[0] = 13;\n  %v_val[1] = 14;\n  %v_val[2] = 15;",
      # Label error will be injected by the generator directly replacing ^latch:
      "latch": "%loop_idx = %loop_idx + 1;\n  %v_val[3] = 16;\n  %v_val[0] = 17;",
      "post_split": "%v_val[1] = 18;\n  %v_val[2] = 19;\n  %v_val[3] = 20;",
      "post_left": "%v_val[0] = 21;\n  %v_val[1] = 22;\n  %v_val[2] = 23;",
      "post_right": "%v_val[3] = 24;\n  %v_val[0] = 25;\n  %v_val[1] = 26;",
      "exit": "%v_val[2] = 27;\n  %v_val[3] = 28;\n  %v_val[0] = 29;",
    },
  },
]

# Write Lexer invalid tests
for idx, info in enumerate(lexer_invalid, start=17):
  topol = info["topology"]
  is_fp = info["is_fp"]
  gen_func = TOPOLOGY_MAP[topol]
  intent = INVALID_INTENTIONS["lexer"][idx]
  sir_code = gen_func(
    info["scen"], is_fp, f"// EXPECT: {info['expect']}\n// Intention: {intent}\n"
  )
  if idx == 21:
    # Inject the lexer error by replacing a label with a bad character
    sir_code = sir_code.replace("^latch:", "^latch?:")
  filename = f"{TEST_DIRS['lexer']}/new_lexer_test_{idx:02d}.sir"
  with open(filename, "w") as f:
    f.write(sir_code)

# 2. Parser Invalid Cases
# Fail at parsing (syntactic error)
parser_invalid = [
  # 17: Topology C - Missing comma in brace init
  {
    "topology": "C",
    "is_fp": False,
    "expect": "FAIL:ParseError",
    "scen": {
      "locals": "let mut %x: <4> i32 = {1 2, 3, 4}; // Parser error: missing comma",
      "entry": "%x[0] = 1;\n  %x[1] = 2;\n  %x[2] = 3;",
      "pre1": "%x[3] = 4;\n  %v_val[0] = 5;\n  %v_val[1] = 6;",
      "pre2": "%branch_cond = cmp < %one_i32, %two_i32;\n  %v_val[2] = 7;\n  %v_val[3] = 8;",
      "loop_cond": "%cond = cmp < %loop_idx, %two;\n  %v_val[0] = 9;\n  %v_val[1] = 10;",
      "body_start": "%v_val[2] = 11;\n  %v_val[3] = 12;\n  %v_val[0] = 13;",
      "body_then": "%x[0] = 5;\n  %v_val[1] = 14;\n  %v_val[2] = 15;",
      "body_else": "%x[0] = 6;\n  %v_val[1] = 16;\n  %v_val[2] = 17;",
      "body_merge": "%x[0] = 7;\n  %v_val[3] = 18;\n  %v_val[0] = 19;",
      "latch": "%loop_idx = %loop_idx + 1;\n  %v_val[1] = 20;\n  %v_val[2] = 21;",
      "exit": "%v_val[3] = 22;\n  %v_val[0] = 23;\n  %v_val[1] = 24;",
      "post1": "%v_val[2] = 25;\n  %v_val[3] = 26;\n  %v_val[0] = 27;",
    },
  },
  # 18: Topology D - Double brackets in lane access
  {
    "topology": "D",
    "is_fp": False,
    "expect": "FAIL:ParseError",
    "scen": {
      "locals": "let mut %x: <4> i32 = 0;",
      "entry": "%x[0] = 1;\n  %x[1] = 2;\n  %x[2] = 3;",
      "outer_cond": "%cond = cmp < %loop_idx, %two;\n  %inner_cond_val = cmp < %inner_idx, %two;\n  %v_val[0] = 4;",
      "outer_body1": "%x[3] = 4;\n  %v_val[1] = 5;\n  %v_val[2] = 6;",
      "inner_cond": "%inner_cond_val = cmp < %inner_idx, %two;\n  %v_val[3] = 7;\n  %v_val[0] = 8;",
      "inner_body1": "%x[[0]] = 10; // Parser error: double brackets\n  %v_val[1] = 9;\n  %v_val[2] = 10;",
      "inner_body2": "%x[1] = 5;\n  %v_val[3] = 11;\n  %v_val[0] = 12;",
      "inner_latch": "%inner_idx = %inner_idx + 1;\n  %v_val[1] = 13;\n  %v_val[2] = 14;",
      "outer_latch": "%loop_idx = %loop_idx + 1;\n  %inner_idx = 0;\n  %v_val[3] = 15;",
      "exit": "%v_val[0] = 16;\n  %v_val[1] = 17;\n  %v_val[2] = 18;",
      "post1": "%v_val[3] = 19;\n  %v_val[0] = 20;\n  %v_val[1] = 21;",
      "post2": "%v_val[2] = 22;\n  %v_val[3] = 23;\n  %v_val[0] = 24;",
      "post3": "%v_val[1] = 25;\n  %v_val[2] = 26;\n  %v_val[3] = 27;",
    },
  },
  # 19: Topology E - Mismatched vector type syntax
  {
    "topology": "E",
    "is_fp": False,
    "expect": "FAIL:ParseError",
    "scen": {
      "locals": "let mut %x: <4 i32 = 0; // Parser error: missing >",
      "entry": "%x[0] = 1;\n  %v_val[0] = 1;\n  %v_val[1] = 2;",
      "l1_cond": "%cond = cmp < %loop_idx, %two;\n  %cond2 = cmp < %loop_idx2, %two;\n  %v_val[2] = 3;",
      "l1_body": "%x[1] = 2;\n  %v_val[3] = 4;\n  %v_val[0] = 5;",
      "l1_latch": "%loop_idx = %loop_idx + 1;\n  %v_val[1] = 6;\n  %v_val[2] = 7;",
      "mid1": "%v_val[3] = 8;\n  %v_val[0] = 9;\n  %v_val[1] = 10;",
      "mid2": "%v_val[2] = 11;\n  %v_val[3] = 12;\n  %v_val[0] = 13;",
      "l2_cond": "%cond2 = cmp < %loop_idx2, %two;\n  %v_val[1] = 14;\n  %v_val[2] = 15;",
      "l2_body": "%x[1] = 2;\n  %v_val[3] = 16;\n  %v_val[0] = 17;",
      "l2_latch": "%loop_idx2 = %loop_idx2 + 1;\n  %v_val[1] = 18;\n  %v_val[2] = 19;",
      "exit": "%v_val[3] = 20;\n  %v_val[0] = 21;\n  %v_val[1] = 22;",
      "post1": "%v_val[2] = 23;\n  %v_val[3] = 24;\n  %v_val[0] = 25;",
    },
  },
  # 20: Topology F - Malformed select expression
  {
    "topology": "F",
    "is_fp": False,
    "expect": "FAIL:ParseError",
    "scen": {
      "locals": "let mut %x: <4> i32 = 0; let %mask: <4> i1 = 0;",
      "entry": "%x[0] = 1;\n  %v_val[0] = 1;\n  %v_val[1] = 2;",
      "loop_cond": "%cond = cmp < %loop_idx, %two;\n  %early_cond = cmp < %one_i32, %zero_i32;\n  %v_val[2] = 3;",
      "body1": "%x = select %mask, %x; // Parser error: mismatched select arms\n  %v_val[3] = 4;\n  %v_val[0] = 5;",
      "body2": "%v_val[1] = 6;\n  %v_val[2] = 7;\n  %v_val[3] = 8;",
      "latch": "%loop_idx = %loop_idx + 1;\n  %v_val[0] = 9;\n  %v_val[1] = 10;",
      "early_exit": "%v_val[2] = 11;\n  %v_val[3] = 12;\n  %v_val[0] = 13;",
      "exit": "%v_val[1] = 14;\n  %v_val[2] = 15;\n  %v_val[3] = 16;",
    },
  },
  # 21: Topology G - Malformed cmp expression
  {
    "topology": "G",
    "is_fp": False,
    "expect": "FAIL:ParseError",
    "scen": {
      "locals": "let mut %x: <4> i32 = 0; let mut %mask: <4> i1 = 0;",
      "entry": "%pre_cond = cmp < %one_i32, %two_i32;\n  %post_cond = cmp < %two_i32, %one_i32;\n  %v_val[0] = 1;",
      "pre_left": "%x[0] = 1;\n  %v_val[1] = 2;\n  %v_val[2] = 3;",
      "pre_right": "%x[1] = 2;\n  %v_val[3] = 4;\n  %v_val[0] = 5;",
      "pre_merge": "%v_val[1] = 6;\n  %v_val[2] = 7;\n  %v_val[3] = 8;",
      "loop_cond": "%cond = cmp < %loop_idx, %two;\n  %v_val[0] = 9;\n  %v_val[1] = 10;",
      "body1": "%mask = cmp < %x %x; // Parser error: missing comma\n  %v_val[2] = 11;\n  %v_val[3] = 12;",
      "body2": "%v_val[0] = 13;\n  %v_val[1] = 14;\n  %v_val[2] = 15;",
      "latch": "%loop_idx = %loop_idx + 1;\n  %v_val[3] = 16;\n  %v_val[0] = 17;",
      "post_split": "%v_val[1] = 18;\n  %v_val[2] = 19;\n  %v_val[3] = 20;",
      "post_left": "%v_val[0] = 21;\n  %v_val[1] = 22;\n  %v_val[2] = 23;",
      "post_right": "%v_val[3] = 24;\n  %v_val[0] = 25;\n  %v_val[1] = 26;",
      "exit": "%v_val[2] = 27;\n  %v_val[3] = 28;\n  %v_val[0] = 29;",
    },
  },
]

# Write Parser invalid tests
for idx, info in enumerate(parser_invalid, start=17):
  topol = info["topology"]
  is_fp = info["is_fp"]
  gen_func = TOPOLOGY_MAP[topol]
  intent = INVALID_INTENTIONS["parser"][idx]
  sir_code = gen_func(
    info["scen"], is_fp, f"// EXPECT: {info['expect']}\n// Intention: {intent}\n"
  )
  filename = f"{TEST_DIRS['parser']}/new_parser_test_{idx:02d}.sir"
  with open(filename, "w") as f:
    f.write(sir_code)

# 3. Typechecker Invalid Cases
# Pass parsing, but fail typechecking
typechecker_invalid = [
  # 17: Topology C - Mixing mismatched vector sizes
  {
    "topology": "C",
    "is_fp": False,
    "expect": "FAIL:StaticError",
    "scen": {
      "locals": "let mut %x: <4> i32 = 0; let %y: <3> i32 = 0;",
      "entry": "%x[0] = 1;\n  %x[1] = 2;\n  %x[2] = 3;",
      "pre1": "%x[3] = 4;\n  %v_val[0] = 5;\n  %v_val[1] = 6;",
      "pre2": "%branch_cond = cmp < %one_i32, %two_i32;\n  %v_val[2] = 7;\n  %v_val[3] = 8;",
      "loop_cond": "%cond = cmp < %loop_idx, %two;\n  %v_val[0] = 9;\n  %v_val[1] = 10;",
      "body_start": "%v_val[2] = 11;\n  %v_val[3] = 12;\n  %v_val[0] = 13;",
      "body_then": "%x = %x + %y; // Type error: <4> i32 + <3> i32\n  %v_val[1] = 14;\n  %v_val[2] = 15;",
      "body_else": "%x[0] = 6;\n  %v_val[1] = 16;\n  %v_val[2] = 17;",
      "body_merge": "%x[0] = 7;\n  %v_val[3] = 18;\n  %v_val[0] = 19;",
      "latch": "%loop_idx = %loop_idx + 1;\n  %v_val[1] = 20;\n  %v_val[2] = 21;",
      "exit": "%v_val[3] = 22;\n  %v_val[0] = 23;\n  %v_val[1] = 24;",
      "post1": "%v_val[2] = 25;\n  %v_val[3] = 26;\n  %v_val[0] = 27;",
    },
  },
  # 18: Topology D - Pointer to vector type (invalid type definition)
  {
    "topology": "D",
    "is_fp": False,
    "expect": "FAIL:StaticError",
    "scen": {
      # LetDecl with pointer to vector fails typecheck/semcheck
      "locals": "let %x: ptr <4> i32 = null; // Type error: pointer to vector",
      "entry": "%v_val[0] = 1;\n  %v_val[1] = 2;\n  %v_val[2] = 3;",
      "outer_cond": "%cond = cmp < %loop_idx, %two;\n  %inner_cond_val = cmp < %inner_idx, %two;\n  %v_val[0] = 4;",
      "outer_body1": "%v_val[1] = 5;\n  %v_val[2] = 6;\n  %v_val[3] = 7;",
      "inner_cond": "%inner_cond_val = cmp < %inner_idx, %two;\n  %v_val[3] = 7;\n  %v_val[0] = 8;",
      "inner_body1": "%v_val[1] = 9;\n  %v_val[2] = 10;\n  %v_val[3] = 11;",
      "inner_body2": "%v_val[3] = 11;\n  %v_val[0] = 12;\n  %v_val[1] = 13;",
      "inner_latch": "%inner_idx = %inner_idx + 1;\n  %v_val[1] = 13;\n  %v_val[2] = 14;",
      "outer_latch": "%loop_idx = %loop_idx + 1;\n  %inner_idx = 0;\n  %v_val[3] = 15;",
      "exit": "%v_val[0] = 16;\n  %v_val[1] = 17;\n  %v_val[2] = 18;",
      "post1": "%v_val[3] = 19;\n  %v_val[0] = 20;\n  %v_val[1] = 21;",
      "post2": "%v_val[2] = 22;\n  %v_val[3] = 23;\n  %v_val[0] = 24;",
      "post3": "%v_val[1] = 25;\n  %v_val[2] = 26;\n  %v_val[3] = 27;",
    },
  },
  # 19: Topology E - Incompatible cast size
  {
    "topology": "E",
    "is_fp": False,
    "expect": "FAIL:StaticError",
    "scen": {
      "locals": "let mut %x: <4> i32 = 0; let mut %y: <3> i32 = 0;",
      "entry": "%x[0] = 1;\n  %v_val[0] = 1;\n  %v_val[1] = 2;",
      "l1_cond": "%cond = cmp < %loop_idx, %two;\n  %cond2 = cmp < %loop_idx2, %two;\n  %v_val[2] = 3;",
      "l1_body": "%y = %x as <3> i32; // Type error: incompatible cast size\n  %v_val[3] = 4;\n  %v_val[0] = 5;",
      "l1_latch": "%loop_idx = %loop_idx + 1;\n  %v_val[1] = 6;\n  %v_val[2] = 7;",
      "mid1": "%v_val[3] = 8;\n  %v_val[0] = 9;\n  %v_val[1] = 10;",
      "mid2": "%v_val[2] = 11;\n  %v_val[3] = 12;\n  %v_val[0] = 13;",
      "l2_cond": "%cond2 = cmp < %loop_idx2, %two;\n  %v_val[1] = 14;\n  %v_val[2] = 15;",
      "l2_body": "%x[1] = 2;\n  %v_val[3] = 16;\n  %v_val[0] = 17;",
      "l2_latch": "%loop_idx2 = %loop_idx2 + 1;\n  %v_val[1] = 18;\n  %v_val[2] = 19;",
      "exit": "%v_val[3] = 20;\n  %v_val[0] = 21;\n  %v_val[1] = 22;",
      "post1": "%v_val[2] = 23;\n  %v_val[3] = 24;\n  %v_val[0] = 25;",
    },
  },
  # 20: Topology F - Bitwise operation on float vector (Exposes Bug: Typechecker accepts bitwise operations on float vectors)
  {
    "topology": "F",
    "is_fp": True,
    "expect": "PASS",
    "scen": {
      "locals": "let mut %x: <4> f32 = 0.0; let %y: <4> f32 = 1.0;",
      "entry": "%x[0] = 1.0;\n  %f_val[0] = 1.0;\n  %f_val[1] = 2.0;",
      "loop_cond": "%cond = cmp < %loop_idx, %two;\n  %early_cond = cmp < %one_f32, %zero_f32;\n  %f_val[2] = 3.0;",
      "body1": "%x = %x & %y; // Type error: bitwise & on float vector\n  %f_val[3] = 4.0;\n  %f_val[0] = 5.0;",
      "body2": "%f_val[1] = 6.0;\n  %f_val[2] = 7.0;\n  %f_val[3] = 8.0;",
      "latch": "%loop_idx = %loop_idx + 1;\n  %f_val[0] = 9.0;\n  %f_val[1] = 10.0;",
      "early_exit": "%f_val[2] = 11.0;\n  %f_val[3] = 12.0;\n  %f_val[0] = 13.0;",
      "exit": "%f_val[1] = 14.0;\n  %f_val[2] = 15.0;\n  %f_val[3] = 16.0;",
    },
  },
  # 21: Topology G - Mismatched select arms
  {
    "topology": "G",
    "is_fp": False,
    "expect": "FAIL:StaticError",
    "scen": {
      "locals": "let mut %x: <4> i32 = 0; let %y: <4> f32 = 0.0; let %mask: <4> i1 = 0;",
      "entry": "%pre_cond = cmp < %one_i32, %two_i32;\n  %post_cond = cmp < %two_i32, %one_i32;\n  %v_val[0] = 1;",
      "pre_left": "%x[0] = 1;\n  %v_val[1] = 2;\n  %v_val[2] = 3;",
      "pre_right": "%x[1] = 2;\n  %v_val[3] = 4;\n  %v_val[0] = 5;",
      "pre_merge": "%v_val[1] = 6;\n  %v_val[2] = 7;\n  %v_val[3] = 8;",
      "loop_cond": "%cond = cmp < %loop_idx, %two;\n  %v_val[0] = 9;\n  %v_val[1] = 10;",
      "body1": "%x = select %mask, %x, %y; // Type error: mismatched select arm types\n  %v_val[2] = 11;\n  %v_val[3] = 12;",
      "body2": "%v_val[0] = 13;\n  %v_val[1] = 14;\n  %v_val[2] = 15;",
      "latch": "%loop_idx = %loop_idx + 1;\n  %v_val[3] = 16;\n  %v_val[0] = 17;",
      "post_split": "%v_val[1] = 18;\n  %v_val[2] = 19;\n  %v_val[3] = 20;",
      "post_left": "%v_val[0] = 21;\n  %v_val[1] = 22;\n  %v_val[2] = 23;",
      "post_right": "%v_val[3] = 24;\n  %v_val[0] = 25;\n  %v_val[1] = 26;",
      "exit": "%v_val[2] = 27;\n  %v_val[3] = 28;\n  %v_val[0] = 29;",
    },
  },
]

# Write Typechecker invalid tests
for idx, info in enumerate(typechecker_invalid, start=17):
  topol = info["topology"]
  is_fp = info["is_fp"]
  gen_func = TOPOLOGY_MAP[topol]
  intent = INVALID_INTENTIONS["typechecker"][idx]
  sir_code = gen_func(
    info["scen"], is_fp, f"// EXPECT: {info['expect']}\n// Intention: {intent}\n"
  )
  filename = f"{TEST_DIRS['typechecker']}/new_typechecker_test_{idx:02d}.sir"
  with open(filename, "w") as f:
    f.write(sir_code)

# 4. Semchecker Invalid Cases
# Pass typechecking, fail semantic checking
semchecker_invalid = [
  # 17: Topology C - Mutating an immutable local/parameter in loop
  {
    "topology": "C",
    "is_fp": False,
    "expect": "FAIL:StaticError",
    "scen": {
      "locals": "let %x: <4> i32 = 0; // Immutable local",
      "entry": "%v_val[0] = 1;\n  %v_val[1] = 2;\n  %v_val[2] = 3;",
      "pre1": "%v_val[0] = 5;\n  %v_val[1] = 6;\n  %v_val[2] = 7;",
      "pre2": "%branch_cond = cmp < %one_i32, %two_i32;\n  %v_val[2] = 7;\n  %v_val[3] = 8;",
      "loop_cond": "%cond = cmp < %loop_idx, %two;\n  %v_val[0] = 9;\n  %v_val[1] = 10;",
      "body_start": "%v_val[2] = 11;\n  %v_val[3] = 12;\n  %v_val[0] = 13;",
      "body_then": "%x = %v_val; // Semcheck error: mutating immutable local\n  %v_val[1] = 14;\n  %v_val[2] = 15;",
      "body_else": "%v_val[1] = 16;\n  %v_val[2] = 17;\n  %v_val[3] = 18;",
      "body_merge": "%v_val[3] = 18;\n  %v_val[0] = 19;\n  %v_val[1] = 20;",
      "latch": "%loop_idx = %loop_idx + 1;\n  %v_val[1] = 20;\n  %v_val[2] = 21;",
      "exit": "%v_val[3] = 22;\n  %v_val[0] = 23;\n  %v_val[1] = 24;",
      "post1": "%v_val[2] = 25;\n  %v_val[3] = 26;\n  %v_val[0] = 27;",
    },
  },
  # 18: Topology D - taking address of a vector
  {
    "topology": "D",
    "is_fp": False,
    "expect": "FAIL:StaticError",
    "scen": {
      "locals": "let mut %x: <4> i32 = 0; let mut %ptr_val: ptr <4> i32 = null; // Staged to pass typecheck",
      "entry": "%x[0] = 1;\n  %x[1] = 2;\n  %x[2] = 3;",
      "outer_cond": "%cond = cmp < %loop_idx, %two;\n  %inner_cond_val = cmp < %inner_idx, %two;\n  %v_val[0] = 4;",
      "outer_body1": "%x[3] = 4;\n  %v_val[1] = 5;\n  %v_val[2] = 6;",
      "inner_cond": "%inner_cond_val = cmp < %inner_idx, %two;\n  %v_val[3] = 7;\n  %v_val[0] = 8;",
      "inner_body1": "%ptr_val = addr %x; // Semcheck error: vectors are not addressable\n  %v_val[1] = 9;\n  %v_val[2] = 10;",
      "inner_body2": "%x[1] = 5;\n  %v_val[3] = 11;\n  %v_val[0] = 12;",
      "inner_latch": "%inner_idx = %inner_idx + 1;\n  %v_val[1] = 13;\n  %v_val[2] = 14;",
      "outer_latch": "%loop_idx = %loop_idx + 1;\n  %inner_idx = 0;\n  %v_val[3] = 15;",
      "exit": "%v_val[0] = 16;\n  %v_val[1] = 17;\n  %v_val[2] = 18;",
      "post1": "%v_val[3] = 19;\n  %v_val[0] = 20;\n  %v_val[1] = 21;",
      "post2": "%v_val[2] = 22;\n  %v_val[3] = 23;\n  %v_val[0] = 24;",
      "post3": "%v_val[1] = 25;\n  %v_val[2] = 26;\n  %v_val[3] = 27;",
    },
  },
  # 19: Topology E - Definite initialization failure (reading undef lane)
  {
    "topology": "E",
    "is_fp": False,
    "expect": "FAIL:StaticError",
    "scen": {
      "locals": "let mut %x: <4> i32 = undef; let mut %y: i32 = 0;",
      "entry": "%y = 1;\n  %v_val[0] = 1;\n  %v_val[1] = 2;",
      "l1_cond": "%cond = cmp < %loop_idx, %two;\n  %cond2 = cmp < %loop_idx2, %two;\n  %v_val[2] = 3;",
      "l1_body": "%y = %x[0]; // Semcheck error: reading undef vector lane\n  %v_val[3] = 4;\n  %v_val[0] = 5;",
      "l1_latch": "%loop_idx = %loop_idx + 1;\n  %v_val[1] = 6;\n  %v_val[2] = 7;",
      "mid1": "%v_val[3] = 8;\n  %v_val[0] = 9;\n  %v_val[1] = 10;",
      "mid2": "%v_val[2] = 11;\n  %v_val[3] = 12;\n  %v_val[0] = 13;",
      "l2_cond": "%cond2 = cmp < %loop_idx2, %two;\n  %v_val[1] = 14;\n  %v_val[2] = 15;",
      "l2_body": "%y = 2;\n  %v_val[3] = 16;\n  %v_val[0] = 17;",
      "l2_latch": "%loop_idx2 = %loop_idx2 + 1;\n  %v_val[1] = 18;\n  %v_val[2] = 19;",
      "exit": "%v_val[3] = 20;\n  %v_val[0] = 21;\n  %v_val[1] = 22;",
      "post1": "%v_val[2] = 23;\n  %v_val[3] = 24;\n  %v_val[0] = 25;",
    },
  },
  # 20: Topology F - Vector type in struct field
  {
    "topology": "F",
    "is_fp": False,
    "expect": "FAIL:StaticError",
    "scen": {
      "locals": "",  # Struct declared outside function, but we can cause a semantic checker error or place struct declaration in header
      "entry": "%v_val[0] = 1;\n  %v_val[1] = 2;\n  %v_val[2] = 3;",
      "loop_cond": "%cond = cmp < %loop_idx, %two;\n  %early_cond = cmp < %one_i32, %zero_i32;\n  %v_val[2] = 3;",
      "body1": "%v_val[0] = 4;\n  %v_val[1] = 5;\n  %v_val[2] = 6;",
      "body2": "%v_val[1] = 6;\n  %v_val[2] = 7;\n  %v_val[3] = 8;",
      "latch": "%loop_idx = %loop_idx + 1;\n  %v_val[0] = 9;\n  %v_val[1] = 10;",
      "early_exit": "%v_val[2] = 11;\n  %v_val[3] = 12;\n  %v_val[0] = 13;",
      "exit": "%v_val[1] = 14;\n  %v_val[2] = 15;\n  %v_val[3] = 16;",
    },
  },
  # 21: Topology G - Duplicate local variable declaration
  {
    "topology": "G",
    "is_fp": False,
    "expect": "FAIL:StaticError",
    "scen": {
      "locals": "let mut %x: <4> i32 = 0; let mut %x: <4> i32 = 0; // Semcheck error: duplicate decl",
      "entry": "%pre_cond = cmp < %one_i32, %two_i32;\n  %post_cond = cmp < %two_i32, %one_i32;\n  %v_val[0] = 1;",
      "pre_left": "%x[0] = 1;\n  %v_val[1] = 2;\n  %v_val[2] = 3;",
      "pre_right": "%x[1] = 2;\n  %v_val[3] = 4;\n  %v_val[0] = 5;",
      "pre_merge": "%v_val[1] = 6;\n  %v_val[2] = 7;\n  %v_val[3] = 8;",
      "loop_cond": "%cond = cmp < %loop_idx, %two;\n  %v_val[0] = 9;\n  %v_val[1] = 10;",
      "body1": "%x[2] = 3;\n  %v_val[2] = 11;\n  %v_val[3] = 12;",
      "body2": "%v_val[0] = 13;\n  %v_val[1] = 14;\n  %v_val[2] = 15;",
      "latch": "%loop_idx = %loop_idx + 1;\n  %v_val[3] = 16;\n  %v_val[0] = 17;",
      "post_split": "%v_val[1] = 18;\n  %v_val[2] = 19;\n  %v_val[3] = 20;",
      "post_left": "%v_val[0] = 21;\n  %v_val[1] = 22;\n  %v_val[2] = 23;",
      "post_right": "%v_val[3] = 24;\n  %v_val[0] = 25;\n  %v_val[1] = 26;",
      "exit": "%v_val[2] = 27;\n  %v_val[3] = 28;\n  %v_val[0] = 29;",
    },
  },
]

# Write Semchecker invalid tests
for idx, info in enumerate(semchecker_invalid, start=17):
  topol = info["topology"]
  is_fp = info["is_fp"]
  gen_func = TOPOLOGY_MAP[topol]
  intent = INVALID_INTENTIONS["semchecker"][idx]
  extra = f"// EXPECT: {info['expect']}\n// Intention: {intent}\n"
  if idx == 20:
    # Prepend a struct declaration with a vector field to cause Semcheck error
    extra += "struct @S {\n  x: <4> i32;\n}\n"
  sir_code = gen_func(info["scen"], is_fp, extra)
  filename = f"{TEST_DIRS['semchecker']}/new_semchecker_test_{idx:02d}.sir"
  with open(filename, "w") as f:
    f.write(sir_code)

# 5. Interpreter Invalid Cases
# Pass typecheck and semcheck, but trigger runtime UB (or EXPECT FAIL:UndefinedBehavior)
# We SKIP: COMPILER because dynamic UB does not fail compilation or crash standard C binaries in simple ways.
interp_invalid = [
  # 17: Topology C - Vector lane subscript index out of bounds
  {
    "topology": "C",
    "is_fp": False,
    "expect": "FAIL:UndefinedBehavior",
    "scen": {
      "locals": "let mut %x: <4> i32 = 0; let mut %idx_val: i32 = 5;",
      "entry": "%x[0] = 1;\n  %x[1] = 2;\n  %x[2] = 3;",
      "pre1": "%x[3] = 4;\n  %v_val[0] = 5;\n  %v_val[1] = 6;",
      "pre2": "%branch_cond = cmp < %one_i32, %two_i32;\n  %v_val[2] = 7;\n  %v_val[3] = 8;",
      "loop_cond": "%cond = cmp < %loop_idx, %two;\n  %v_val[0] = 9;\n  %v_val[1] = 10;",
      "body_start": "%v_val[2] = 11;\n  %v_val[3] = 12;\n  %v_val[0] = 13;",
      "body_then": "%v_val[0] = %x[%idx_val]; // Runtime UB: index 5 is out of bounds [0, 4)\n  %v_val[1] = 14;\n  %v_val[2] = 15;",
      "body_else": "%v_val[1] = 16;\n  %v_val[2] = 17;\n  %v_val[3] = 18;",
      "body_merge": "%v_val[3] = 18;\n  %v_val[0] = 19;\n  %v_val[1] = 20;",
      "latch": "%loop_idx = %loop_idx + 1;\n  %v_val[1] = 20;\n  %v_val[2] = 21;",
      "exit": "%v_val[3] = 22;\n  %v_val[0] = 23;\n  %v_val[1] = 24;",
      "post1": "%v_val[2] = 25;\n  %v_val[3] = 26;\n  %v_val[0] = 27;",
    },
  },
  # 18: Topology D - Lane-wise division by zero
  {
    "topology": "D",
    "is_fp": False,
    "expect": "FAIL:UndefinedBehavior",
    "scen": {
      "locals": "let mut %x: <4> i32 = {1, 2, 3, 4}; let %zeros: <4> i32 = {1, 0, 1, 1}; let mut %r: <4> i32 = 0;",
      "entry": "%x[0] = 1;\n  %x[1] = 2;\n  %x[2] = 3;",
      "outer_cond": "%cond = cmp < %loop_idx, %two;\n  %inner_cond_val = cmp < %inner_idx, %two;\n  %v_val[0] = 4;",
      "outer_body1": "%x[3] = 4;\n  %v_val[1] = 5;\n  %v_val[2] = 6;",
      "inner_cond": "%inner_cond_val = cmp < %inner_idx, %two;\n  %v_val[3] = 7;\n  %v_val[0] = 8;",
      "inner_body1": "%r = %x / %zeros; // Runtime UB: division by zero in lane 1\n  %v_val[1] = 9;\n  %v_val[2] = 10;",
      "inner_body2": "%x[1] = 5;\n  %v_val[3] = 11;\n  %v_val[0] = 12;",
      "inner_latch": "%inner_idx = %inner_idx + 1;\n  %v_val[1] = 13;\n  %v_val[2] = 14;",
      "outer_latch": "%loop_idx = %loop_idx + 1;\n  %inner_idx = 0;\n  %v_val[3] = 15;",
      "exit": "%v_val[0] = 16;\n  %v_val[1] = 17;\n  %v_val[2] = 18;",
      "post1": "%v_val[3] = 19;\n  %v_val[0] = 20;\n  %v_val[1] = 21;",
      "post2": "%v_val[2] = 22;\n  %v_val[3] = 23;\n  %v_val[0] = 24;",
      "post3": "%v_val[1] = 25;\n  %v_val[2] = 26;\n  %v_val[3] = 27;",
    },
  },
  # 19: Topology E - Signed integer overflow in vector addition
  {
    "topology": "E",
    "is_fp": False,
    "expect": "FAIL:UndefinedBehavior",
    "scen": {
      "locals": "let mut %x: <4> i32 = {2147483640, 2, 3, 4}; let %y: <4> i32 = {10, 0, 0, 0}; let mut %r: <4> i32 = 0;",
      "entry": "%x[1] = 2;\n  %v_val[0] = 1;\n  %v_val[1] = 2;",
      "l1_cond": "%cond = cmp < %loop_idx, %two;\n  %cond2 = cmp < %loop_idx2, %two;\n  %v_val[2] = 3;",
      "l1_body": "%r = %x + %y; // Runtime UB: signed overflow in lane 0 (2147483640 + 10)\n  %v_val[3] = 4;\n  %v_val[0] = 5;",
      "l1_latch": "%loop_idx = %loop_idx + 1;\n  %v_val[1] = 6;\n  %v_val[2] = 7;",
      "mid1": "%v_val[3] = 8;\n  %v_val[0] = 9;\n  %v_val[1] = 10;",
      "mid2": "%v_val[2] = 11;\n  %v_val[3] = 12;\n  %v_val[0] = 13;",
      "l2_cond": "%cond2 = cmp < %loop_idx2, %two;\n  %v_val[1] = 14;\n  %v_val[2] = 15;",
      "l2_body": "%x[1] = 2;\n  %v_val[3] = 16;\n  %v_val[0] = 17;",
      "l2_latch": "%loop_idx2 = %loop_idx2 + 1;\n  %v_val[1] = 18;\n  %v_val[2] = 19;",
      "exit": "%v_val[3] = 20;\n  %v_val[0] = 21;\n  %v_val[1] = 22;",
      "post1": "%v_val[2] = 23;\n  %v_val[3] = 24;\n  %v_val[0] = 25;",
    },
  },
  # 20: Topology F - Out-of-bounds float-to-int cast
  {
    "topology": "F",
    "is_fp": True,
    "expect": "FAIL:UndefinedBehavior",
    "scen": {
      "locals": "let mut %x: <4> f32 = {1e15, 2.0, 3.0, 4.0}; let mut %r: <4> i32 = 0;",
      "entry": "%x[1] = 2.0;\n  %f_val[0] = 1.0;\n  %f_val[1] = 2.0;",
      "loop_cond": "%cond = cmp < %loop_idx, %two;\n  %early_cond = cmp < %one_f32, %zero_f32;\n  %f_val[2] = 3.0;",
      "body1": "%r = %x as <4> i32; // Runtime UB: 1e15 is outside the representable range of i32\n  %f_val[3] = 4.0;\n  %f_val[0] = 5.0;",
      "body2": "%f_val[1] = 6.0;\n  %f_val[2] = 7.0;\n  %f_val[3] = 8.0;",
      "latch": "%loop_idx = %loop_idx + 1;\n  %f_val[0] = 9.0;\n  %f_val[1] = 10.0;",
      "early_exit": "%f_val[2] = 11.0;\n  %f_val[3] = 12.0;\n  %f_val[0] = 13.0;",
      "exit": "%f_val[1] = 14.0;\n  %f_val[2] = 15.0;\n  %f_val[3] = 16.0;",
    },
  },
  # 21: Topology G - Reading from undefined vector lane
  {
    "topology": "G",
    "is_fp": False,
    "expect": "FAIL:UndefinedBehavior",
    "scen": {
      # LetDecl with undef, staging to bypass semchecker static checks by writing some lanes, then reading an unwritten undef lane
      "locals": "let mut %x: <4> i32 = undef; let mut %y: i32 = 0;",
      "entry": "%x[0] = 1;\n  %x[1] = 2;\n  %v_val[0] = 1;",
      "pre_left": "%x[2] = 3;\n  %v_val[1] = 2;\n  %v_val[2] = 3;",
      "pre_right": "%x[2] = 4;\n  %v_val[3] = 4;\n  %v_val[0] = 5;",
      "pre_merge": "%v_val[1] = 6;\n  %v_val[2] = 7;\n  %v_val[3] = 8;",
      "loop_cond": "%cond = cmp < %loop_idx, %two;\n  %v_val[0] = 9;\n  %v_val[1] = 10;",
      # Lane 3 is never written, so reading it is UB
      "body1": "%y = %x[3]; // Runtime UB: read undef lane\n  %v_val[2] = 11;\n  %v_val[3] = 12;",
      "body2": "%v_val[0] = 13;\n  %v_val[1] = 14;\n  %v_val[2] = 15;",
      "latch": "%loop_idx = %loop_idx + 1;\n  %v_val[3] = 16;\n  %v_val[0] = 17;",
      "post_split": "%v_val[1] = 18;\n  %v_val[2] = 19;\n  %v_val[3] = 20;",
      "post_left": "%v_val[0] = 21;\n  %v_val[1] = 22;\n  %v_val[2] = 23;",
      "post_right": "%v_val[3] = 24;\n  %v_val[0] = 25;\n  %v_val[1] = 26;",
      "exit": "%v_val[2] = 27;\n  %v_val[3] = 28;\n  %v_val[0] = 29;",
    },
  },
]

# Write Interpreter invalid tests
for idx, info in enumerate(interp_invalid, start=17):
  topol = info["topology"]
  is_fp = info["is_fp"]
  gen_func = TOPOLOGY_MAP[topol]
  intent = INVALID_INTENTIONS["interp"][idx]
  sir_code = gen_func(
    info["scen"],
    is_fp,
    f"// EXPECT: {info['expect']}\n// Intention: {intent}\n// SKIP: COMPILER\n",
  )
  filename = f"{TEST_DIRS['interp']}/new_interp_test_{idx:02d}.sir"
  with open(filename, "w") as f:
    f.write(sir_code)

# 6. Compiler Invalid Cases
# Statically invalid, checked by typecheck/semcheck before compilation
compiler_invalid = [
  # 17: Topology C - Writing to immutable vector local
  {
    "topology": "C",
    "is_fp": False,
    "expect": "FAIL:StaticError",
    "scen": {
      "locals": "let %x: <4> i32 = 0;",
      "entry": "%v_val[0] = 1;\n  %v_val[1] = 2;\n  %v_val[2] = 3;",
      "pre1": "%v_val[0] = 5;\n  %v_val[1] = 6;\n  %v_val[2] = 7;",
      "pre2": "%branch_cond = cmp < %one_i32, %two_i32;\n  %v_val[2] = 7;\n  %v_val[3] = 8;",
      "loop_cond": "%cond = cmp < %loop_idx, %two;\n  %v_val[0] = 9;\n  %v_val[1] = 10;",
      "body_start": "%v_val[2] = 11;\n  %v_val[3] = 12;\n  %v_val[0] = 13;",
      "body_then": "%x = %v_val; // Static error: mutating immutable local\n  %v_val[1] = 14;\n  %v_val[2] = 15;",
      "body_else": "%v_val[1] = 16;\n  %v_val[2] = 17;\n  %v_val[3] = 18;",
      "body_merge": "%v_val[3] = 18;\n  %v_val[0] = 19;\n  %v_val[1] = 20;",
      "latch": "%loop_idx = %loop_idx + 1;\n  %v_val[1] = 20;\n  %v_val[2] = 21;",
      "exit": "%v_val[3] = 22;\n  %v_val[0] = 23;\n  %v_val[1] = 24;",
      "post1": "%v_val[2] = 25;\n  %v_val[3] = 26;\n  %v_val[0] = 27;",
    },
  },
  # 18: Topology D - taking address of a vector
  {
    "topology": "D",
    "is_fp": False,
    "expect": "FAIL:StaticError",
    "scen": {
      "locals": "let mut %x: <4> i32 = 0; let mut %ptr_val: ptr <4> i32 = null;",
      "entry": "%x[0] = 1;\n  %x[1] = 2;\n  %x[2] = 3;",
      "outer_cond": "%cond = cmp < %loop_idx, %two;\n  %inner_cond_val = cmp < %inner_idx, %two;\n  %v_val[0] = 4;",
      "outer_body1": "%x[3] = 4;\n  %v_val[1] = 5;\n  %v_val[2] = 6;",
      "inner_cond": "%inner_cond_val = cmp < %inner_idx, %two;\n  %v_val[3] = 7;\n  %v_val[0] = 8;",
      "inner_body1": "%ptr_val = addr %x; // Static error: address of vector\n  %v_val[1] = 9;\n  %v_val[2] = 10;",
      "inner_body2": "%x[1] = 5;\n  %v_val[3] = 11;\n  %v_val[0] = 12;",
      "inner_latch": "%inner_idx = %inner_idx + 1;\n  %v_val[1] = 13;\n  %v_val[2] = 14;",
      "outer_latch": "%loop_idx = %loop_idx + 1;\n  %inner_idx = 0;\n  %v_val[3] = 15;",
      "exit": "%v_val[0] = 16;\n  %v_val[1] = 17;\n  %v_val[2] = 18;",
      "post1": "%v_val[3] = 19;\n  %v_val[0] = 20;\n  %v_val[1] = 21;",
      "post2": "%v_val[2] = 22;\n  %v_val[3] = 23;\n  %v_val[0] = 24;",
      "post3": "%v_val[1] = 25;\n  %v_val[2] = 26;\n  %v_val[3] = 27;",
    },
  },
  # 19: Topology E - Struct with vector field
  {
    "topology": "E",
    "is_fp": False,
    "expect": "FAIL:StaticError",
    "scen": {
      "locals": "",
      "entry": "%v_val[0] = 1;\n  %v_val[1] = 2;\n  %v_val[2] = 3;",
      "l1_cond": "%cond = cmp < %loop_idx, %two;\n  %cond2 = cmp < %loop_idx2, %two;\n  %v_val[2] = 3;",
      "l1_body": "%v_val[3] = 4;\n  %v_val[0] = 5;\n  %v_val[1] = 6;",
      "l1_latch": "%loop_idx = %loop_idx + 1;\n  %v_val[1] = 6;\n  %v_val[2] = 7;",
      "mid1": "%v_val[3] = 8;\n  %v_val[0] = 9;\n  %v_val[1] = 10;",
      "mid2": "%v_val[2] = 11;\n  %v_val[3] = 12;\n  %v_val[0] = 13;",
      "l2_cond": "%cond2 = cmp < %loop_idx2, %two;\n  %v_val[1] = 14;\n  %v_val[2] = 15;",
      "l2_body": "%v_val[3] = 16;\n  %v_val[0] = 17;\n  %v_val[1] = 18;",
      "l2_latch": "%loop_idx2 = %loop_idx2 + 1;\n  %v_val[1] = 18;\n  %v_val[2] = 19;",
      "exit": "%v_val[3] = 20;\n  %v_val[0] = 21;\n  %v_val[1] = 22;",
      "post1": "%v_val[2] = 23;\n  %v_val[3] = 24;\n  %v_val[0] = 25;",
    },
  },
  # 20: Topology F - Vector size cast mismatch
  {
    "topology": "F",
    "is_fp": False,
    "expect": "FAIL:StaticError",
    "scen": {
      "locals": "let mut %x: <4> i32 = 0; let mut %y: <3> i32 = 0;",
      "entry": "%x[0] = 1;\n  %v_val[0] = 1;\n  %v_val[1] = 2;",
      "loop_cond": "%cond = cmp < %loop_idx, %two;\n  %early_cond = cmp < %one_i32, %zero_i32;\n  %v_val[2] = 3;",
      "body1": "%y = %x as <3> i32; // Static error: cast size mismatch\n  %v_val[3] = 4;\n  %v_val[0] = 5;",
      "body2": "%v_val[1] = 6;\n  %v_val[2] = 7;\n  %v_val[3] = 8;",
      "latch": "%loop_idx = %loop_idx + 1;\n  %v_val[0] = 9;\n  %v_val[1] = 10;",
      "early_exit": "%v_val[2] = 11;\n  %v_val[3] = 12;\n  %v_val[0] = 13;",
      "exit": "%v_val[1] = 14;\n  %v_val[2] = 15;\n  %v_val[3] = 16;",
    },
  },
  # 21: Topology G - Mismatched select types
  {
    "topology": "G",
    "is_fp": False,
    "expect": "FAIL:StaticError",
    "scen": {
      "locals": "let mut %x: <4> i32 = 0; let %y: <4> f32 = 0.0; let %mask: <4> i1 = 0;",
      "entry": "%pre_cond = cmp < %one_i32, %two_i32;\n  %post_cond = cmp < %two_i32, %one_i32;\n  %v_val[0] = 1;",
      "pre_left": "%x[0] = 1;\n  %v_val[1] = 2;\n  %v_val[2] = 3;",
      "pre_right": "%x[1] = 2;\n  %v_val[3] = 4;\n  %v_val[0] = 5;",
      "pre_merge": "%v_val[1] = 6;\n  %v_val[2] = 7;\n  %v_val[3] = 8;",
      "loop_cond": "%cond = cmp < %loop_idx, %two;\n  %v_val[0] = 9;\n  %v_val[1] = 10;",
      "body1": "%x = select %mask, %x, %y; // Static error: select types mismatch\n  %v_val[2] = 11;\n  %v_val[3] = 12;",
      "body2": "%v_val[0] = 13;\n  %v_val[1] = 14;\n  %v_val[2] = 15;",
      "latch": "%loop_idx = %loop_idx + 1;\n  %v_val[3] = 16;\n  %v_val[0] = 17;",
      "post_split": "%v_val[1] = 18;\n  %v_val[2] = 19;\n  %v_val[3] = 20;",
      "post_left": "%v_val[0] = 21;\n  %v_val[1] = 22;\n  %v_val[2] = 23;",
      "post_right": "%v_val[3] = 24;\n  %v_val[0] = 25;\n  %v_val[1] = 26;",
      "exit": "%v_val[2] = 27;\n  %v_val[3] = 28;\n  %v_val[0] = 29;",
    },
  },
]

# Write Compiler invalid tests
for idx, info in enumerate(compiler_invalid, start=17):
  topol = info["topology"]
  is_fp = info["is_fp"]
  gen_func = TOPOLOGY_MAP[topol]
  intent = INVALID_INTENTIONS["compile"][idx]
  extra = f"// EXPECT: {info['expect']}\n// Intention: {intent}\n// COMPILER_ARGS:\n"
  if idx == 19:
    extra += "struct @S {\n  x: <4> i32;\n}\n"
  sir_code = gen_func(info["scen"], is_fp, extra)
  filename = f"{TEST_DIRS['compile']}/new_compile_test_{idx:02d}.sir"
  with open(filename, "w") as f:
    f.write(sir_code)

# 7. Solver Invalid Cases
# Statically valid, but unsatisfiable (unsat) or contain path UB
solver_invalid = [
  # 17: Topology C - Contradictory constraints (unsat)
  {
    "topology": "C",
    "is_fp": False,
    "expect": "FAIL",
    "scen": {
      "locals": "sym %?v: value <4> i32 in [0, 10]; let mut %x: <4> i32 = 0;",
      "entry": "%x = %?v;\n  %v_val[0] = 1;\n  %v_val[1] = 2;",
      "pre1": "%v_val[0] = 5;\n  %v_val[1] = 6;\n  %v_val[2] = 7;",
      "pre2": "%branch_cond = cmp < %one_i32, %two_i32;\n  %v_val[2] = 7;\n  %v_val[3] = 8;",
      "loop_cond": "%cond = cmp < %loop_idx, %two;\n  %v_val[0] = 9;\n  %v_val[1] = 10;",
      "body_start": "%v_val[2] = 11;\n  %v_val[3] = 12;\n  %v_val[0] = 13;",
      "body_then": "require %x[0] == 5;\n  require %x[0] == 6; // Unsat: %x[0] cannot be both 5 and 6\n  %v_val[1] = 14;",
      "body_else": "%v_val[1] = 16;\n  %v_val[2] = 17;\n  %v_val[3] = 18;",
      "body_merge": "%v_val[3] = 18;\n  %v_val[0] = 19;\n  %v_val[1] = 20;",
      "latch": "%loop_idx = %loop_idx + 1;\n  %v_val[1] = 20;\n  %v_val[2] = 21;",
      "exit": "%v_val[3] = 22;\n  %v_val[0] = 23;\n  %v_val[1] = 24;",
      "post1": "%v_val[2] = 25;\n  %v_val[3] = 26;\n  %v_val[0] = 27;",
    },
  },
  # 18: Topology D - Path forcing vector lane OOB
  {
    "topology": "D",
    "is_fp": False,
    "expect": "FAIL:UndefinedBehavior",
    "scen": {
      "locals": "sym %?i: index i32 in [0, 10]; let mut %x: <4> i32 = 0; let mut %y: i32 = 0;",
      "entry": "%x[0] = 1;\n  %x[1] = 2;\n  %x[2] = 3;",
      "outer_cond": "%cond = cmp < %loop_idx, %two;\n  %inner_cond_val = cmp < %inner_idx, %two;\n  %v_val[0] = 4;",
      "outer_body1": "%x[3] = 4;\n  %v_val[1] = 5;\n  %v_val[2] = 6;",
      "inner_cond": "%inner_cond_val = cmp < %inner_idx, %two;\n  %v_val[3] = 7;\n  %v_val[0] = 8;",
      "inner_body1": "assume %?i == 5;\n  %y = %x[%?i]; // Forced UB: %?i is 5, causing lane OOB\n  %v_val[1] = 9;",
      "inner_body2": "%x[1] = 5;\n  %v_val[3] = 11;\n  %v_val[0] = 12;",
      "inner_latch": "%inner_idx = %inner_idx + 1;\n  %v_val[1] = 13;\n  %v_val[2] = 14;",
      "outer_latch": "%loop_idx = %loop_idx + 1;\n  %inner_idx = 0;\n  %v_val[3] = 15;",
      "exit": "%v_val[0] = 16;\n  %v_val[1] = 17;\n  %v_val[2] = 18;",
      "post1": "%v_val[3] = 19;\n  %v_val[0] = 20;\n  %v_val[1] = 21;",
      "post2": "%v_val[2] = 22;\n  %v_val[3] = 23;\n  %v_val[0] = 24;",
      "post3": "%v_val[1] = 25;\n  %v_val[2] = 26;\n  %v_val[3] = 27;",
    },
  },
  # 19: Topology E - Path forcing lane division by zero
  {
    "topology": "E",
    "is_fp": False,
    "expect": "FAIL:UndefinedBehavior",
    "scen": {
      "locals": "sym %?d: value i32 in [0, 5]; let mut %x: <4> i32 = {1, 2, 3, 4}; let mut %div: <4> i32 = 0; let mut %r: <4> i32 = 0;",
      "entry": "%x[1] = 2;\n  %v_val[0] = 1;\n  %v_val[1] = 2;",
      "l1_cond": "%cond = cmp < %loop_idx, %two;\n  %cond2 = cmp < %loop_idx2, %two;\n  %v_val[2] = 3;",
      "l1_body": "assume %?d == 0;\n  %div[0] = %?d; %div[1] = 1; %div[2] = 1; %div[3] = 1;\n  %r = %x / %div; // Forced UB: division by zero in lane 0\n  %v_val[0] = 5;",
      "l1_latch": "%loop_idx = %loop_idx + 1;\n  %v_val[1] = 6;\n  %v_val[2] = 7;",
      "mid1": "%v_val[3] = 8;\n  %v_val[0] = 9;\n  %v_val[1] = 10;",
      "mid2": "%v_val[2] = 11;\n  %v_val[3] = 12;\n  %v_val[0] = 13;",
      "l2_cond": "%cond2 = cmp < %loop_idx2, %two;\n  %v_val[1] = 14;\n  %v_val[2] = 15;",
      "l2_body": "%x[1] = 2;\n  %v_val[3] = 16;\n  %v_val[0] = 17;",
      "l2_latch": "%loop_idx2 = %loop_idx2 + 1;\n  %v_val[1] = 18;\n  %v_val[2] = 19;",
      "exit": "%v_val[3] = 20;\n  %v_val[0] = 21;\n  %v_val[1] = 22;",
      "post1": "%v_val[2] = 23;\n  %v_val[3] = 24;\n  %v_val[0] = 25;",
    },
  },
  # 20: Topology F - Path forcing out-of-range float cast
  {
    "topology": "F",
    "is_fp": True,
    "expect": "FAIL:UndefinedBehavior",
    "scen": {
      "locals": "sym %?f: value f32; let mut %x: <4> f32 = 0.0; let mut %r: <4> i32 = 0;",
      "entry": "%x[1] = 2.0;\n  %f_val[0] = 1.0;\n  %f_val[1] = 2.0;",
      "loop_cond": "%cond = cmp < %loop_idx, %two;\n  %early_cond = cmp < %one_f32, %zero_f32;\n  %f_val[2] = 3.0;",
      "body1": "assume %?f > 1e12;\n  %x[0] = %?f;\n  %r = %x as <4> i32; // Forced UB: cast out of range\n  %f_val[0] = 5.0;",
      "body2": "%f_val[1] = 6.0;\n  %f_val[2] = 7.0;\n  %f_val[3] = 8.0;",
      "latch": "%loop_idx = %loop_idx + 1;\n  %f_val[0] = 9.0;\n  %f_val[1] = 10.0;",
      "early_exit": "%f_val[2] = 11.0;\n  %f_val[3] = 12.0;\n  %f_val[0] = 13.0;",
      "exit": "%f_val[1] = 14.0;\n  %f_val[2] = 15.0;\n  %f_val[3] = 16.0;",
    },
  },
  # 21: Topology G - Path forcing signed integer overflow
  {
    "topology": "G",
    "is_fp": False,
    "expect": "FAIL:UndefinedBehavior",
    "scen": {
      "locals": "sym %?v: value i32; let mut %x: <4> i32 = {2147483640, 2, 3, 4}; let mut %y: <4> i32 = 0; let mut %r: <4> i32 = 0;",
      "entry": "%pre_cond = cmp < %one_i32, %two_i32;\n  %post_cond = cmp < %two_i32, %one_i32;\n  %v_val[0] = 1;",
      "pre_left": "%x[0] = 2147483640;\n  %v_val[1] = 2;\n  %v_val[2] = 3;",
      "pre_right": "%x[1] = 2;\n  %v_val[3] = 4;\n  %v_val[0] = 5;",
      "pre_merge": "%v_val[1] = 6;\n  %v_val[2] = 7;\n  %v_val[3] = 8;",
      "loop_cond": "%cond = cmp < %loop_idx, %two;\n  %v_val[0] = 9;\n  %v_val[1] = 10;",
      "body1": "assume %?v == 10;\n  %y[0] = %?v;\n  %r = %x + %y; // Forced UB: signed overflow\n  %v_val[3] = 12;",
      "body2": "%v_val[0] = 13;\n  %v_val[1] = 14;\n  %v_val[2] = 15;",
      "latch": "%loop_idx = %loop_idx + 1;\n  %v_val[3] = 16;\n  %v_val[0] = 17;",
      "post_split": "%v_val[1] = 18;\n  %v_val[2] = 19;\n  %v_val[3] = 20;",
      "post_left": "%v_val[0] = 21;\n  %v_val[1] = 22;\n  %v_val[2] = 23;",
      "post_right": "%v_val[3] = 24;\n  %v_val[0] = 25;\n  %v_val[1] = 26;",
      "exit": "%v_val[2] = 27;\n  %v_val[3] = 28;\n  %v_val[0] = 29;",
    },
  },
]

# Write Solver invalid tests
for idx, info in enumerate(solver_invalid, start=17):
  topol = info["topology"]
  is_fp = info["is_fp"]
  gen_func = TOPOLOGY_MAP[topol]
  path_str = ""
  if topol == "A":
    path_str = "^entry,^loop_cond,^body1,^body2,^body3,^body4,^latch,^loop_cond,^body1,^body2,^body3,^body4,^latch,^loop_cond,^exit,^post1,^post2,^post3"
  elif topol == "B":
    path_str = "^entry,^pre1,^pre2,^pre3,^pre4,^pre5,^pre6,^loop_cond,^body1,^body2,^body3,^latch,^loop_cond,^body1,^body2,^body3,^latch,^loop_cond,^exit"
  elif topol == "C":
    path_str = "^entry,^pre1,^pre2,^loop_cond,^body_start,^body_then,^body_merge,^latch,^loop_cond,^body_start,^body_then,^body_merge,^latch,^loop_cond,^exit,^post1"
  elif topol == "D":
    path_str = "^entry,^outer_cond,^outer_body1,^inner_cond,^inner_body1,^inner_body2,^inner_latch,^inner_cond,^inner_body1,^inner_body2,^inner_latch,^inner_cond,^outer_latch,^outer_cond,^exit,^post1,^post2,^post3"
  elif topol == "E":
    path_str = "^entry,^l1_cond,^l1_body,^l1_latch,^l1_cond,^l1_body,^l1_latch,^l1_cond,^mid1,^mid2,^l2_cond,^l2_body,^l2_latch,^l2_cond,^l2_body,^l2_latch,^l2_cond,^exit,^post1"
  elif topol == "F":
    path_str = "^entry,^loop_cond,^body1,^body2,^latch,^loop_cond,^body1,^body2,^latch,^loop_cond,^exit"
  elif topol == "G":
    path_str = "^entry,^pre_left,^pre_merge,^loop_cond,^body1,^body2,^latch,^loop_cond,^body1,^body2,^latch,^loop_cond,^post_split,^post_left,^exit"
  intent = INVALID_INTENTIONS["solver"][idx]
  extra = f"// EXPECT: {info['expect']}\n// Intention: {intent}\n// SOLVER_ARGS: --main @main --path '{path_str}'\n"
  sir_code = gen_func(info["scen"], is_fp, extra)
  filename = f"{TEST_DIRS['solver']}/new_solver_test_{idx:02d}.sir"
  with open(filename, "w") as f:
    f.write(sir_code)

print("Generated all 147 test cases successfully!")
