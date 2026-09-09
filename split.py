#!/usr/bin/env python3
import re
import sys
from pathlib import Path

# Mappatura dei rinominamenti (Vecchio Nome -> Nuovo Nome)
RENAMES = {
    # 🔴 Problemi reali
    "build_dag": "dag_build",
    
    # ⚠️ Moduli con incoerenza reale
    # sr.c
    "count_variable_definitions": "sr_count_variable_definitions",
    "collect_base_induction_vars": "sr_collect_base_induction_vars",
    "findInductionBase": "sr_find_induction_base",
    "match_iv_mul_const": "sr_match_iv_mul_const",
    "try_match_derived_iv": "sr_try_match_derived_iv",
    "find_derived_induction_vars": "sr_find_derived_induction_vars",
    "emit_preheader_inits": "sr_emit_preheader_inits",
    "patch_body_instruction": "sr_patch_body_instruction",
    "rewrite_loop_body": "sr_rewrite_loop_body",
    "apply_strength_reduction": "sr_apply_strength_reduction",

    # loop.c
    "init_dominator_sets": "loop_init_dominator_sets",
    "intersect_predecessor_dominators": "loop_intersect_predecessor_dominators",
    "update_dominator_set": "loop_update_dominator_set",
    "bfs_traverse_loop_body": "loop_bfs_traverse_loop_body",
    "collectBody": "loop_collect_body",
    "record_loop_exit_blocks": "loop_record_loop_exit_blocks",
    "build_natural_loop": "loop_build_natural_loop",
    "create_pre_header_block": "loop_create_pre_header_block",
    "reroute_non_body_predecessors": "loop_reroute_non_body_predecessors",

    # regalloc.c
    "build_cfg": "regalloc_build_cfg",
    "finalize": "regalloc_finalize",
    "save_restore_callee": "regalloc_save_restore_callee",
    "wire_block_successors": "regalloc_wire_block_successors",
    "build_label_to_block": "regalloc_build_label_to_block",

    # sched.c
    "find_basic_blocks": "sched_find_basic_blocks",
    "emit_pinned_headers": "sched_emit_pinned_headers",
    "seed_ready_heap": "sched_seed_ready_heap",
    "unlock_successors": "sched_unlock_successors",
    "run_list_scheduling": "sched_run_list_scheduling",
    "flush_leftovers": "sched_flush_leftovers",
    "schedule_block": "sched_schedule_block",

    # sched_dag.c
    "rename_tracker_create": "tracker_create",
    "rename_tracker_track_read": "tracker_read",
    "rename_tracker_track_write": "tracker_write",
    "rename_tracker_track_reg_list_read": "tracker_reg_list_read",
    "rename_tracker_track_reg_list_write": "tracker_reg_list_write",

    # ra_color.c
    "build_partner_index": "ra_build_partner_index",
    "compute_forbidden_colors": "ra_compute_forbidden_colors",
    "choose_color": "ra_choose_color",

    # ra_coalesce.c
    "resolve_operand_id": "ra_resolve_operand_id",
    "is_valid_coalesce_candidate": "ra_is_valid_coalesce_candidate",

    # global_lower.c
    "function_touches_globals": "gl_function_touches_globals",

    # instr_selector.c
    "load_operand": "isel_load_operand",
    "operand_to_mach": "isel_operand_to_mach",
    "find_global_idx": "isel_find_global_idx",
    "flip_cmp": "isel_flip_cmp",
    "comparison_to_setcc": "isel_comparison_to_setcc",
    "flush_pending_cmp": "isel_flush_pending_cmp",

    # ir.c (minori)
    "mk_var": "ir_mk_var",
    "compact_block": "ir_compact_block",

    # constmap.c
    "fold_unary": "lat_fold_unary",
}

def fix_cp_comment_and_linkage(content: str) -> str:
    """Corregge il commento errato e rende static is_comparison_op se non lo è."""
    # Correggi il commento fuorviante in cp.c
    old_comment = "arithmetic kernel lives in constmap.c"
    new_comment = "arithmetic kernel lives locally in fold_binary_int/fold_binary_float"
    content = content.replace(old_comment, new_comment)

    # Rendi static la funzione is_comparison_op in cp.c (se definita non-static)
    content = re.sub(
        r'^(bool\s+is_comparison_op\s*\()',
        r'static \1',
        content,
        flags=re.MULTILINE
    )
    return content

def apply_refactoring(file_path: Path):
    try:
        content = file_path.read_text(encoding="utf-8")
    except Exception as e:
        print(f"[!] Impossibile leggere {file_path}: {e}")
        return

    original_content = content

    # Correggi cp.c specificatamente per commento e linkage
    if file_path.name == "cp.c":
        content = fix_cp_comment_and_linkage(content)

    # Applica il rinominamento dei simboli rispettando i confini delle parole (\b)
    for old_name, new_name in RENAMES.items():
        pattern = r'\b' + re.escape(old_name) + r'\b'
        content = re.sub(pattern, new_name, content)

    if content != original_content:
        file_path.write_text(content, encoding="utf-8")
        print(f"[✓] Modificato: {file_path}")

def main():
    # Cerca ricorsivamente file .c e .h nella cartella corrente e sottocartelle
    project_root = Path(".")
    files = list(project_root.glob("**/*.[ch]"))
    
    if not files:
        print("[!] Nessun file .c o .h trovato nella directory corrente.")
        sys.exit(1)

    print(f"Processando {len(files)} file sorgente...")
    for f in files:
        apply_refactoring(f)

    print("\nRefactoring completato con successo!")

if __name__ == "__main__":
    main()