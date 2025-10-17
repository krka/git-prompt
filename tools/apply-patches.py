#!/usr/bin/env python3
"""
Git source patch runner for git-prompt optimization.

Reads patches.yaml and applies source patches to enable LTO dead code elimination.
Fails explicitly if any pattern doesn't match (detects Git version changes).
"""

import sys
import re
import yaml
from pathlib import Path
from typing import List, Dict, Tuple

def load_patches(yaml_path: Path) -> Dict:
    """Load patch configuration from YAML file."""
    try:
        with open(yaml_path) as f:
            return yaml.safe_load(f)
    except FileNotFoundError:
        print(f"Error: Patch configuration not found: {yaml_path}", file=sys.stderr)
        sys.exit(1)
    except yaml.YAMLError as e:
        print(f"Error: Invalid YAML in {yaml_path}: {e}", file=sys.stderr)
        sys.exit(1)

def find_and_patch_line(lines: List[str], pattern: str, action: str) -> Tuple[bool, int]:
    r"""
    Find pattern in lines and apply patch action.

    Automatically handles leading whitespace by prepending ^\s* to the pattern.
    This allows clean patterns in YAML without worrying about tabs vs spaces.

    Returns: (success, line_number) where line_number is 1-indexed
    """
    # Build regex: start of line + any whitespace + user pattern
    # Escape special regex chars in user pattern
    escaped_pattern = re.escape(pattern)
    regex_pattern = r'^\s*' + escaped_pattern

    for i, line in enumerate(lines):
        if re.search(regex_pattern, line):
            if action == 'comment':
                # Comment out the line - preserve indentation
                indent = len(line) - len(line.lstrip())
                comment = '//' if not line.lstrip().startswith('//') else ''
                lines[i] = ' ' * indent + comment + ' ' + line.lstrip()
            elif action == 'remove':
                lines[i] = ''  # Remove line entirely
            else:
                print(f"Error: Unknown action '{action}'", file=sys.stderr)
                return False, -1
            return True, i + 1
    return False, -1

def apply_patches(submodule_dir: Path, patch_config: Dict) -> int:
    """
    Apply all patches from configuration.

    Returns: number of patches applied, or -1 on error
    """
    patches_applied = 0

    for file_config in patch_config.get('patches', []):
        file_path = submodule_dir / file_config['file']
        description = file_config.get('description', 'No description')

        print(f"\n📝 {file_config['file']}: {description}")

        # Read file
        if not file_path.exists():
            print(f"  ❌ Error: File not found: {file_path}", file=sys.stderr)
            return -1

        with open(file_path) as f:
            lines = f.readlines()

        original_lines = lines.copy()
        file_patches_applied = 0

        # Apply each patch for this file
        for patch in file_config.get('patches', []):
            pattern = patch['pattern']
            action = patch['action']
            reason = patch.get('reason', 'No reason specified')

            success, line_num = find_and_patch_line(lines, pattern, action)

            if success:
                print(f"  ✓ Line {line_num}: {action} - {reason}")
                file_patches_applied += 1
            else:
                print(f"  ❌ Error: Pattern not found in {file_path}", file=sys.stderr)
                print(f"     Pattern: {pattern}", file=sys.stderr)
                print(f"     This may indicate a Git version change.", file=sys.stderr)
                return -1

        # Write patched file if changes were made
        if file_patches_applied > 0:
            with open(file_path, 'w') as f:
                f.writelines(lines)
            patches_applied += file_patches_applied
        else:
            print(f"  ℹ️  No patches applied to this file")

    return patches_applied

def main():
    """Main entry point."""
    # Determine paths
    script_dir = Path(__file__).parent
    project_root = script_dir.parent
    yaml_path = script_dir / 'patches.yaml'

    # Allow override of submodule directory
    submodule_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else project_root / 'submodules' / 'git'

    if not submodule_dir.exists():
        print(f"Error: Git submodule directory not found: {submodule_dir}", file=sys.stderr)
        sys.exit(1)

    print("=" * 60)
    print("Git Source Patch Runner")
    print("=" * 60)
    print(f"Submodule: {submodule_dir}")
    print(f"Config: {yaml_path}")

    # Load and apply patches
    patch_config = load_patches(yaml_path)
    patches_applied = apply_patches(submodule_dir, patch_config)

    if patches_applied < 0:
        print("\n❌ Patching failed!", file=sys.stderr)
        sys.exit(1)

    print(f"\n✅ Successfully applied {patches_applied} patches")
    print("\nRun 'make optimized' to build with patches applied")

if __name__ == '__main__':
    main()
