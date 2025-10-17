#!/usr/bin/python3
"""
Git Prompt Test Runner

Runs declarative tests from test_cases.yaml in sequence, building up git state
progressively in a temporary directory.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile
import yaml
from pathlib import Path
from collections import defaultdict
import html as html_escape


class Colors:
    """ANSI color codes for terminal output"""
    GREEN = '\033[92m'
    RED = '\033[91m'
    YELLOW = '\033[93m'
    BLUE = '\033[94m'
    RESET = '\033[0m'
    BOLD = '\033[1m'


def run_command(cmd, cwd, verbose=False):
    """Run a shell command and return output"""
    if verbose:
        print(f"    $ {cmd}")

    # Set fixed Git environment variables for deterministic commits
    env = os.environ.copy()
    env.update({
        'GIT_AUTHOR_NAME': 'Test User',
        'GIT_AUTHOR_EMAIL': 'test@example.com',
        'GIT_COMMITTER_NAME': 'Test User',
        'GIT_COMMITTER_EMAIL': 'test@example.com',
        'GIT_AUTHOR_DATE': '2020-01-01T00:00:00Z',
        'GIT_COMMITTER_DATE': '2020-01-01T00:00:00Z',
    })

    result = subprocess.run(
        cmd,
        shell=True,
        cwd=cwd,
        capture_output=True,
        text=True,
        env=env
    )

    if verbose and result.stdout:
        print(f"      {result.stdout.rstrip()}")
    if verbose and result.stderr:
        print(f"      stderr: {result.stderr.rstrip()}")

    return result.returncode, result.stdout, result.stderr


def get_git_prompt_output(git_prompt_path, cwd, with_color=False, large_repo_size=None, max_traversal=None):
    """Get output from git-prompt"""
    color_flag = "" if with_color else "--no-color"
    size_flag = f"--large-repo-size={large_repo_size}" if large_repo_size is not None else ""
    traversal_flag = f"--max-traversal={max_traversal}" if max_traversal is not None else "--max-traversal=10"
    local_flag = "--local"  # Always use --local in tests to avoid global config interference
    returncode, stdout, stderr = run_command(
        f"{git_prompt_path} {color_flag} {size_flag} {traversal_flag} {local_flag}".strip(),
        cwd=cwd,
        verbose=False
    )
    return stdout.rstrip()


def match_output(actual, expected):
    """
    Match actual output against expected, supporting variables:
    - $NUMBER: any integer
    - $ANYTHING: any non-whitespace string

    Color markers like {GREEN}, {RED}, etc. should be included in expected string.
    """
    # Escape regex special chars except our variables
    pattern = re.escape(expected)

    # Replace our variables with regex patterns
    pattern = pattern.replace(r'\$NUMBER', r'\d+')
    pattern = pattern.replace(r'\$ANYTHING', r'\S+')

    # Exact match
    return re.fullmatch(pattern, actual) is not None


def ansi_to_markers(text):
    """
    Convert ANSI color codes to readable markers like {GREEN}text{}
    Pattern: \001\033[01;XXm\002 or \001\033[00m\002 (with Bash readline escape markers)
    """
    color_map = {
        '32': 'GREEN',
        '31': 'RED',
        '33': 'YELLOW',
        '34': 'BLUE',
        '35': 'MAGENTA',
        '36': 'CYAN',
        '37': 'GRAY',
    }

    result = text
    # Replace ANSI color start codes with {COLOR}
    result = re.sub(r'\x01\x1b\[01;(\d+)m\x02', lambda m: f'{{{color_map.get(m.group(1), "UNKNOWN")}}}', result)
    # Replace ANSI color end codes with {}
    result = re.sub(r'\x01\x1b\[00m\x02', '{}', result)

    return result


def markers_to_ansi(text):
    """
    Convert readable markers like {GREEN}text{} back to ANSI codes
    """
    color_map = {
        'GREEN': '32',
        'RED': '31',
        'YELLOW': '33',
        'BLUE': '34',
        'MAGENTA': '35',
        'CYAN': '36',
        'GRAY': '37',
    }

    result = text
    # Replace {COLOR} with ANSI color start codes
    for name, code in color_map.items():
        result = result.replace(f'{{{name}}}', f'\x01\x1b[01;{code}m\x02')
    # Replace {} with ANSI color end codes
    result = result.replace('{}', '\x01\x1b[00m\x02')

    return result


def ansi_to_html(text):
    """Convert ANSI color codes to HTML spans using ANSI color numbers"""
    # Map ANSI color codes to ANSI palette numbers
    # These will use the ghostty palette colors via CSS classes
    color_map = {
        '32': 'ansi2',   # Green (bright green in palette)
        '31': 'ansi1',   # Red
        '33': 'ansi3',   # Yellow
        '34': 'ansi4',   # Blue
        '35': 'ansi5',   # Magenta
        '36': 'ansi6',   # Cyan
        '37': 'ansi7',   # White
    }

    # Escape HTML first
    text = html_escape.escape(text)

    # Replace ANSI codes with HTML spans
    # Pattern: \001\033[01;XXm\002 or \001\033[00m\x002 (with Bash readline escape markers)
    result = text
    result = re.sub(r'\x01\x1b\[01;(\d+)m\x02', lambda m: f'<span class="{color_map.get(m.group(1), "ansi7")}">', result)
    result = re.sub(r'\x01\x1b\[00m\x02', '</span>', result)

    return result


def generate_text_report(test_results, output_path):
    """Generate a detailed text report for test results"""
    lines = []

    # Calculate summary stats
    total = len(test_results)
    passed = sum(1 for r in test_results if r['passed'])
    failed = total - passed
    pass_rate = (passed / total * 100) if total > 0 else 0

    # Header
    lines.append("=" * 80)
    lines.append("Git Prompt Test Results")
    lines.append("=" * 80)
    lines.append("")
    lines.append(f"Test Summary: {passed}/{total} passed ({pass_rate:.1f}%)")
    if failed > 0:
        lines.append(f"              {failed} failed")
    lines.append("")
    lines.append("=" * 80)
    lines.append("")

    # Add each test case
    for i, result in enumerate(test_results, 1):
        status_text = "PASSED" if result['passed'] else "FAILED"
        status_symbol = "✓" if result['passed'] else "✗"

        lines.append(f"{status_symbol} {status_text} [{i}/{total}] {result['name']}")

        # Add setup steps if present
        if result.get('steps'):
            lines.append("")
            lines.append("  Setup Steps:")
            for step_info in result['steps']:
                repeat = step_info.get('repeat', 1)
                if repeat > 1:
                    lines.append(f"    Repeated {repeat} times:")
                    lines.append(f"      $ {step_info['command']}")
                else:
                    lines.append(f"    $ {step_info['command']}")
                if step_info.get('error'):
                    lines.append(f"      Error: {step_info['error']}")

        # Add prompt output section
        lines.append("")
        if result['passed']:
            # For passed tests, show both the expected pattern and the actual colored output
            lines.append(f"  Expected: {result['expected']}")
            lines.append(f"  Actual:   {result['actual']}")
        else:
            # For failed tests, show expected and actual
            lines.append(f"  Expected: {result['expected']}")
            lines.append(f"  Actual:   {result['actual']}")

        lines.append("")
        lines.append("-" * 80)
        lines.append("")

    # Footer summary
    lines.append("=" * 80)
    if failed == 0:
        lines.append(f"All tests passed! ({passed}/{total})")
    else:
        lines.append(f"{passed} passed, {failed} failed ({pass_rate:.1f}%)")
    lines.append("=" * 80)

    # Write to file
    with open(output_path, 'w') as f:
        f.write('\n'.join(lines))

    return output_path


def generate_detailed_report(test_results, output_path):
    """Generate a detailed HTML report with all test steps and results"""
    html = """<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Git Prompt Test Results</title>
    <link rel="stylesheet" href="test-styles.css">
    <style>
        .summary {
            background-color: #252526;
            border-left: 4px solid #4ec9b0;
            padding: 15px 20px;
            margin-bottom: 30px;
            border-radius: 4px;
        }
        .summary.failed {
            border-left-color: #cc6666;
        }
        .test-case {
            background-color: #252526;
            border-radius: 8px;
            padding: 0;
            margin-bottom: 10px;
            border: 1px solid #3e3e42;
        }
        .test-case.passed {
            border-left: 4px solid #8abf68;
        }
        .test-case.failed {
            border-left: 4px solid #cc6666;
        }
        .test-header {
            display: flex;
            align-items: center;
            padding: 15px 20px;
            cursor: pointer;
            user-select: none;
            gap: 12px;
        }
        .test-header:hover {
            background-color: #2d2d30;
        }
        .test-name {
            font-size: 1.1em;
            font-weight: 600;
            color: #4ec9b0;
            flex: 1;
        }
        .expand-icon {
            color: #858585;
            font-size: 0.9em;
            transition: transform 0.2s;
            margin-right: 8px;
        }
        .test-case.expanded .expand-icon {
            transform: rotate(90deg);
        }
        .test-details {
            display: none;
            padding: 0 20px 20px 20px;
        }
        .test-case.expanded .test-details {
            display: block;
        }
        .test-status {
            font-weight: bold;
            padding: 4px 12px;
            border-radius: 4px;
            font-size: 0.9em;
        }
        .test-status.passed {
            background-color: #8abf68;
            color: #1e1e1e;
        }
        .test-status.failed {
            background-color: #cc6666;
            color: #1e1e1e;
        }
        .steps-section {
            margin: 15px 0;
        }
        .steps-title {
            color: #b294bb;
            font-weight: 600;
            margin-bottom: 10px;
            font-size: 0.95em;
            cursor: pointer;
            user-select: none;
            display: flex;
            align-items: center;
            gap: 8px;
        }
        .steps-title:hover {
            color: #c9a7d1;
        }
        .steps-expand-icon {
            color: #858585;
            font-size: 0.85em;
            transition: transform 0.2s;
        }
        .steps-section.expanded .steps-expand-icon {
            transform: rotate(90deg);
        }
        .steps-content {
            display: none;
        }
        .steps-section.expanded .steps-content {
            display: block;
        }
        .step {
            background-color: #1d1f21;
            border: 1px solid #3e3e42;
            border-radius: 4px;
            padding: 8px 12px;
            margin-bottom: 6px;
            font-family: 'Consolas', 'Monaco', 'Courier New', monospace;
            font-size: 13px;
        }
        .step.error {
            border-left: 3px solid #cc6666;
            background-color: #2d1f1f;
        }
        .step-command {
            color: #8abeb7;
        }
        .step-error {
            color: #cc6666;
            margin-top: 4px;
            font-size: 0.9em;
        }
        .result-section {
            margin: 15px 0;
        }
        .result-title {
            color: #b294bb;
            font-weight: 600;
            margin-bottom: 10px;
            font-size: 0.95em;
        }
        .output-box {
            background-color: #1d1f21;
            border: 1px solid #3e3e42;
            border-radius: 4px;
            padding: 12px;
            font-family: 'Consolas', 'Monaco', 'Courier New', monospace;
            font-size: 14px;
            margin-bottom: 10px;
        }
        .output-box.expected {
            border-left: 3px solid #8abf68;
        }
        .output-box.actual {
            border-left: 3px solid #cc6666;
        }
        .label {
            color: #858585;
            font-size: 0.85em;
            margin-bottom: 4px;
        }
    </style>
    <script>
        function toggleTest(element) {
            element.closest('.test-case').classList.toggle('expanded');
        }

        function toggleSteps(element) {
            element.closest('.steps-section').classList.toggle('expanded');
        }

        // Expand all failed tests by default
        document.addEventListener('DOMContentLoaded', function() {
            document.querySelectorAll('.test-case.failed').forEach(function(testCase) {
                testCase.classList.add('expanded');
            });
        });
    </script>
</head>
<body>
    <h1>Git Prompt Test Results</h1>
"""

    # Calculate summary stats
    total = len(test_results)
    passed = sum(1 for r in test_results if r['passed'])
    failed = total - passed
    pass_rate = (passed / total * 100) if total > 0 else 0

    summary_class = "summary" if failed == 0 else "summary failed"
    html += f"""
    <div class="{summary_class}">
        <strong>Test Summary:</strong> {passed}/{total} passed ({pass_rate:.1f}%)
        {f" • {failed} failed" if failed > 0 else ""}
    </div>
"""

    # Add each test case
    for result in test_results:
        status_class = "passed" if result['passed'] else "failed"
        status_text = "PASSED" if result['passed'] else "FAILED"

        html += f"""
    <div class="test-case {status_class}">
        <div class="test-header" onclick="toggleTest(this)">
            <span class="expand-icon">▶</span>
            <div class="test-status {status_class}">{status_text}</div>
            <div class="test-name">
                {html_escape.escape(result['name'])}
            </div>
        </div>
        <div class="test-details">
"""

        # Add setup steps
        if result.get('steps'):
            html += """
            <div class="steps-section">
                <div class="steps-title" onclick="toggleSteps(this)">
                    <span class="steps-expand-icon">▶</span>
                    Setup Steps
                </div>
                <div class="steps-content">
"""
            for step_info in result['steps']:
                step_class = "step error" if step_info.get('error') else "step"
                repeat = step_info.get('repeat', 1)
                if repeat > 1:
                    html += f"""                <div class="{step_class}">
                    <div style="color: #858585; font-size: 0.9em; margin-bottom: 4px;">Repeated {repeat} times:</div>
                    <div class="step-command" style="margin-left: 16px;">$ {html_escape.escape(step_info['command'])}</div>
"""
                else:
                    html += f"""                <div class="{step_class}">
                    <div class="step-command">$ {html_escape.escape(step_info['command'])}</div>
"""
                if step_info.get('error'):
                    html += f"""                    <div class="step-error">Error: {html_escape.escape(step_info['error'])}</div>
"""
                html += """                </div>
"""
            html += """                </div>
            </div>
"""

        # Add result section
        html += """
            <div class="result-section">
                <div class="result-title">Prompt Output:</div>
"""

        if result['passed']:
            # Check if we have different large mode output
            if result.get('colored_output_large') and not result.get('is_custom_mode'):
                # Show both small and large mode outputs
                small_output_html = ansi_to_html(result['colored_output']) if result['colored_output'] else '<em>(no output)</em>'
                large_output_html = ansi_to_html(result['colored_output_large'])
                html += f"""                <div class="output-box expected">
                    <div class="label">Small Repo Mode:</div>
                    {small_output_html}
                </div>
                <div class="output-box expected">
                    <div class="label">Large Repo Mode:</div>
                    {large_output_html}
                </div>
"""
            else:
                # Show just expected output (which matches actual)
                output_html = ansi_to_html(result['colored_output']) if result['colored_output'] else '<em>(no output)</em>'
                html += f"""                <div class="output-box expected">
                    <div class="label">Expected & Actual:</div>
                    {output_html}
                </div>
"""
        else:
            # Show both expected and actual with highlighting
            expected_html = html_escape.escape(result['expected']) if result['expected'] else '<em>(no output)</em>'
            actual_html = html_escape.escape(result['actual']) if result['actual'] else '<em>(no output)</em>'
            html += f"""                <div class="output-box expected">
                    <div class="label">Expected:</div>
                    {expected_html}
                </div>
                <div class="output-box actual">
                    <div class="label">Actual:</div>
                    {actual_html}
                </div>
"""

        html += """            </div>
        </div>
    </div>
"""

    html += """
    <div class="footer">
        Generated by git-prompt test suite
    </div>
</body>
</html>
"""

    with open(output_path, 'w') as f:
        f.write(html)

    return output_path


def generate_html_report(test_results, output_path, examples_only=False):
    """Generate an HTML report with test examples grouped by category

    Args:
        test_results: List of test result dictionaries
        output_path: Path to write the HTML file
        examples_only: If True, only include tests marked with example=True
    """
    # Filter to examples only if requested
    if examples_only:
        test_results = [r for r in test_results if r.get('is_example', False)]

    # Group tests by category, then deduplicate by output pair (small, large)
    # Structure: grouped[group][(small_output, large_output)] = [test_names]
    grouped = defaultdict(lambda: defaultdict(list))
    for result in test_results:
        group = result.get('group', 'other')
        small_output = result['colored_output']
        large_output = result.get('colored_output_large', None)
        # Group by output pair within each category
        # Use tuple (small_output, large_output) as key to properly deduplicate
        output_key = (small_output, large_output)
        grouped[group][output_key].append(result['name'])

    # Define group order and titles
    group_info = {
        'basic': 'Basic Git States',
        'working-tree': 'Working Tree States',
        'branches': 'Branches',
        'detached': 'Detached HEAD',
        'upstream': 'Upstream Tracking',
        'stash': 'Stash',
        'in-progress': 'Operations In Progress',
    }

    html = """<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Git Prompt Examples</title>
    <link rel="stylesheet" href="test-styles.css">
    <style>
        .section {
            margin-bottom: 40px;
        }
        table {
            width: 100%;
            border-collapse: collapse;
            margin-bottom: 30px;
        }
        th {
            background-color: #252526;
            color: #cccccc;
            padding: 12px;
            text-align: left;
            border-bottom: 2px solid #3e3e42;
            font-weight: 600;
        }
        td {
            padding: 12px;
            border-bottom: 1px solid #3e3e42;
        }
        tr:hover {
            background-color: #2d2d30;
        }
        .test-name {
            color: #d4d4d4;
            font-size: 0.95em;
        }
    </style>
</head>
<body>
    <h1>Git Prompt Examples</h1>
    <p>Visual examples of git-prompt output for different repository states.</p>
"""

    # Add each group
    for group_key in ['basic', 'working-tree', 'branches', 'detached', 'upstream', 'stash', 'in-progress']:
        if group_key not in grouped:
            continue

        output_to_tests = grouped[group_key]
        group_title = group_info.get(group_key, group_key.title())

        html += f"""
    <div class="section">
        <h2>{group_title}</h2>
        <table>
            <thead>
                <tr>
                    <th style="width: 40%">Test Case(s)</th>
                    <th style="width: 60%">Output</th>
                </tr>
            </thead>
            <tbody>
"""

        # Iterate through unique output pairs
        for output_key, test_names in output_to_tests.items():
            small_output, large_output = output_key
            # Join test names with commas
            names_str = ', '.join(test_names)
            small_output_html = ansi_to_html(small_output) if small_output else '<em>(no output)</em>'

            # Check if we have a different large output
            if large_output is not None and large_output != small_output:
                # Show both small and large mode outputs
                large_output_html = ansi_to_html(large_output)
                html += f"""                <tr>
                    <td class="test-name">{html_escape.escape(names_str)}</td>
                    <td>
                        <div style="margin-bottom: 8px;">
                            <span style="color: #858585; font-size: 0.85em;">Small Repo Mode:</span>
                            <div class="terminal">{small_output_html}</div>
                        </div>
                        <div>
                            <span style="color: #858585; font-size: 0.85em;">Large Repo Mode:</span>
                            <div class="terminal">{large_output_html}</div>
                        </div>
                    </td>
                </tr>
"""
            else:
                # Show just small output
                html += f"""                <tr>
                    <td class="test-name">{html_escape.escape(names_str)}</td>
                    <td><div class="terminal">{small_output_html}</div></td>
                </tr>
"""

        html += """            </tbody>
        </table>
    </div>
"""

    html += """
    <div class="footer">
        Generated by git-prompt test suite
    </div>
</body>
</html>
"""

    with open(output_path, 'w') as f:
        f.write(html)

    print(f"\n{Colors.BLUE}HTML report generated: {output_path}{Colors.RESET}")


def generate_examples_snippet(test_results, output_path):
    """Generate an HTML snippet (just the content, no wrapper) for embedding in documentation

    Args:
        test_results: List of test result dictionaries (should be pre-filtered to examples)
        output_path: Path to write the HTML snippet
    """
    # Collect all examples (deduplicated by output)
    seen_outputs = set()
    examples = []

    for result in test_results:
        if not result.get('is_example', False):
            continue

        small_output = result['colored_output']
        large_output = result.get('colored_output_large', None)
        output_key = (small_output, large_output)

        # Skip duplicates
        if output_key in seen_outputs:
            continue
        seen_outputs.add(output_key)

        examples.append({
            'name': result['name'],
            'small': small_output,
            'large': large_output,
        })

    # Generate a single compact table with 3 columns
    html = '''<table>
    <thead>
        <tr>
            <th style="width: 40%">Description</th>
            <th style="width: 30%">Output</th>
            <th style="width: 30%; font-style: italic;">(If large repo)</th>
        </tr>
    </thead>
    <tbody>
'''

    for example in examples:
        name = html_escape.escape(example['name'])
        small_html = ansi_to_html(example['small']) if example['small'] else '<em>(no output)</em>'

        # Check if we have a different large output
        if example['large'] is not None and example['large'] != example['small']:
            large_html = ansi_to_html(example['large'])
            html += f'''        <tr>
            <td class="test-name">{name}</td>
            <td style="width: 30%"><div class="terminal">{small_html}</div></td>
            <td style="width: 30%; color: #858585;"><div class="terminal">{large_html}</div></td>
        </tr>
'''
        else:
            # No large output difference - span both columns
            html += f'''        <tr>
            <td class="test-name">{name}</td>
            <td colspan="2"><div class="terminal">{small_html}</div></td>
        </tr>
'''

    html += '''    </tbody>
</table>
'''

    # Write snippet to file
    with open(output_path, 'w') as f:
        f.write(html)

    print(f"{Colors.BLUE}HTML snippet generated: {output_path}{Colors.RESET}")


def get_test_binaries(base_path):
    """
    Get all test binaries for the given base path.
    Returns list of (name, path) tuples. All binaries are required to exist.

    The test suite always tests 5 binaries:
    - unpatched (baseline reference)
    - main (shipping binary)
    - patched-debug (for analysis)
    - asan (Address Sanitizer)
    - ubsan (Undefined Behavior Sanitizer)
    """
    base_dir = Path(base_path).parent
    base_name = Path(base_path).stem  # 'git-prompt' without path

    # Normalize base name: strip any known suffixes to get the core binary name
    # This allows passing any variant (git-prompt-asan, git-prompt-unpatched, etc.)
    # and still finding all other variants correctly
    known_suffixes = ['-asan', '-ubsan', '-unpatched', '-patched-debug']
    for suffix in known_suffixes:
        if base_name.endswith(suffix):
            base_name = base_name[:-len(suffix)]
            break

    # All binaries that must exist (built by `make all`)
    binaries = [
        ('unpatched (baseline)', base_dir / f"{base_name}-unpatched"),
        ('main (shipping)', base_dir / base_name),
        ('patched-debug (analysis)', base_dir / f"{base_name}-patched-debug"),
        ('asan', base_dir / f"{base_name}-asan"),
        ('ubsan', base_dir / f"{base_name}-ubsan"),
    ]

    # Verify all binaries exist
    missing = [name for name, path in binaries if not path.exists()]
    if missing:
        raise FileNotFoundError(
            f"Missing binaries: {', '.join(missing)}\n"
            f"Run 'make all' to build all required binaries."
        )

    return binaries


def run_test_suite(test_file, git_prompt_path, verbose=False, replace_expected=False):
    """Run all tests from YAML file"""

    # Get all required test binaries
    try:
        test_binaries = get_test_binaries(git_prompt_path)
        print(f"{Colors.BOLD}Testing with {len(test_binaries)} binaries:{Colors.RESET}")
        for name, path in test_binaries:
            print(f"  - {name}: {path}")
        print()
    except FileNotFoundError as e:
        print(f"{Colors.RED}{e}{Colors.RESET}")
        return False

    # Extract just the paths and names for use in tests
    binary_paths = [path for _, path in test_binaries]
    binary_names = [name for name, _ in test_binaries]

    # Load test cases
    with open(test_file, 'r') as f:
        data = yaml.safe_load(f)

    tests = data.get('tests', [])
    if not tests:
        print(f"{Colors.RED}No tests found in {test_file}{Colors.RESET}")
        return False

    print(f"{Colors.BOLD}Running {len(tests)} tests...{Colors.RESET}\n")

    # Create temporary directory structure for tests
    # - Base directory: /tmp/tmpXXXXXX/
    # - Repository directory: /tmp/tmpXXXXXX/repo/ (becomes pwd for all git commands)
    # - Tests can use relative paths like ../worktree-dir which stay inside the temp structure
    with tempfile.TemporaryDirectory() as tmpdir:
        test_dir = os.path.join(tmpdir, 'repo')
        os.makedirs(test_dir)

        passed = 0
        failed = 0
        test_results = []

        for i, test in enumerate(tests, 1):
            name = test.get('name', f'Test {i}')
            steps = test.get('steps', [])
            expected = test.get('expected', '')
            expected_large = test.get('expected_large', None)
            reset = test.get('reset', False)
            is_example = test.get('example', False)
            # Override large_repo_size: use test-specific value if provided
            test_specific_size = test.get('large_repo_size', None)

            # Track steps for detailed report
            step_results = []
            test_failed_during_setup = False

            # Reset to fresh directory if requested
            if reset:
                if os.path.exists(test_dir):
                    shutil.rmtree(test_dir)
                os.makedirs(test_dir)

            # Execute setup steps
            for step_item in steps:
                # Support both string steps and dict with 'command' and 'repeat'
                if isinstance(step_item, dict):
                    step = step_item['command']
                    repeat = step_item.get('repeat', 1)
                else:
                    step = step_item
                    repeat = 1

                # Track step for detailed report (with repeat count)
                step_info = {'command': step, 'repeat': repeat}

                for _ in range(repeat):
                    returncode, stdout, stderr = run_command(step, test_dir, verbose=verbose)

                    # Most git commands we don't care about errors (e.g., "nothing to commit")
                    # But we should fail on truly broken commands
                    if returncode != 0 and "fatal" in stderr.lower():
                        step_info['error'] = stderr.rstrip()
                        step_results.append(step_info)
                        print(f"{Colors.BOLD}[{i}/{len(tests)}]{Colors.RESET} {name}")
                        print(f"  {Colors.RED}✗ FAILED{Colors.RESET} - Command failed: {step}")
                        print(f"    Error: {stderr.rstrip()}")
                        failed += 1
                        test_failed_during_setup = True
                        break

                if test_failed_during_setup:
                    break
                else:
                    # Only append step_info if no error occurred
                    step_results.append(step_info)

            # Get max_traversal from test (for tests that override it)
            max_traversal = test.get('max_traversal', None)

            # Run tests in both small and large repo modes
            # Use test-specific size if provided, otherwise use defaults
            test_modes = []
            if test_specific_size is not None:
                # Test has specific large_repo_size override - only test that mode
                test_modes.append(('custom', test_specific_size, expected))
            else:
                # Test both small and large modes
                # small mode: high threshold (100MB) = repo treated as small (normal colors)
                test_modes.append(('small', 100000000, expected))
                # large mode: low threshold (1 byte) = repo treated as large (gray, skips status checks)
                large_expected = expected_large if expected_large is not None else expected
                test_modes.append(('large', 1, large_expected))

            all_modes_passed = True
            mode_results = []

            for mode_name, large_repo_size, mode_expected in test_modes:
                # Run all binaries and collect outputs
                binary_outputs = []
                for binary_path in binary_paths:
                    colored = get_git_prompt_output(str(binary_path), test_dir, with_color=True, large_repo_size=large_repo_size, max_traversal=max_traversal)
                    actual = ansi_to_markers(colored)
                    binary_outputs.append((colored, actual))

                # Use first binary (unpatched/baseline) as the reference
                colored_output, actual = binary_outputs[0]

                # Check if all binaries agree
                all_match = all(output[1] == actual for output in binary_outputs)
                diverged_binaries = []
                if not all_match:
                    for idx, (_, output) in enumerate(binary_outputs[1:], 1):
                        if output != actual:
                            diverged_binaries.append((idx, binary_names[idx], output))

                # Compare baseline output against expected
                test_passed = match_output(actual, mode_expected)
                binaries_agree = all_match

                if not (test_passed and binaries_agree):
                    all_modes_passed = False

                mode_results.append({
                    'mode': mode_name,
                    'size': large_repo_size,
                    'expected': mode_expected,
                    'actual': actual,
                    'colored_output': colored_output,
                    'passed': test_passed,
                    'binaries_agree': binaries_agree,
                    'diverged_binaries': diverged_binaries,
                })

            # Print result summary
            if verbose:
                print(f"{Colors.BOLD}[{i}/{len(tests)}]{Colors.RESET} {name}")
                for mode_result in mode_results:
                    print(f"  Mode: {mode_result['mode']}")
                    print(f"    Actual:   {repr(mode_result['actual'])}")
                    print(f"    Expected: {repr(mode_result['expected'])}")
                    if mode_result['diverged_binaries']:
                        for idx, name_str, output in mode_result['diverged_binaries']:
                            print(f"    {name_str} diverged: {repr(output)}")

            if all_modes_passed:
                mode_str = f" ({', '.join(m['mode'] for m in mode_results)})"
                print(f"{Colors.GREEN}✓ PASSED{Colors.RESET} {Colors.BOLD}[{i}/{len(tests)}]{Colors.RESET} {name}{mode_str}")
                passed += 1
            else:
                print(f"{Colors.RED}✗ FAILED{Colors.RESET} {Colors.BOLD}[{i}/{len(tests)}]{Colors.RESET} {name}")
                for mode_result in mode_results:
                    if not mode_result['passed']:
                        print(f"    [{mode_result['mode']}] {binary_names[0]} output:   {Colors.YELLOW}{repr(mode_result['actual'])}{Colors.RESET}")
                        print(f"    [{mode_result['mode']}] Expected:                 {Colors.YELLOW}{repr(mode_result['expected'])}{Colors.RESET}")
                    if not mode_result['binaries_agree']:
                        for idx, name_str, output in mode_result['diverged_binaries']:
                            print(f"    [{mode_result['mode']}] {name_str} diverged: {Colors.RED}{repr(output)}{Colors.RESET}")
                failed += 1

            # Replace expected with actual if requested
            if replace_expected:
                # Handle tests with custom large_repo_size
                if test_specific_size is not None and len(mode_results) > 0:
                    custom_result = mode_results[0]
                    test['expected'] = custom_result['actual']

                # Handle small mode (always at index 0 if not test-specific)
                if test_specific_size is None and len(mode_results) > 0:
                    small_result = mode_results[0]
                    test['expected'] = small_result['actual']

                # Handle large mode (always at index 1 if not test-specific)
                if test_specific_size is None and len(mode_results) > 1:
                    large_result = mode_results[1]
                    small_result = mode_results[0]

                    # Check if large output differs from small output
                    large_differs = large_result['actual'] != small_result['actual']

                    if large_differs:
                        test['expected_large'] = large_result['actual']
                    else:
                        # Large output same as small - remove expected_large if it exists
                        if 'expected_large' in test:
                            del test['expected_large']

            # Store result for reports (HTML and text)
            # Store both small and large mode results for HTML display
            small_result = mode_results[0] if len(mode_results) > 0 else None
            large_result = mode_results[1] if len(mode_results) > 1 else None

            # Determine if small and large outputs differ
            has_different_large = (large_result is not None and
                                  small_result is not None and
                                  large_result['colored_output'] != small_result['colored_output'])

            test_results.append({
                'name': name,
                'group': test.get('group', 'other'),
                'expected': small_result['expected'] if small_result else '',
                'actual': small_result['actual'] if small_result else '',
                'colored_output': small_result['colored_output'] if small_result else '',
                'colored_output_large': large_result['colored_output'] if has_different_large else None,
                'passed': all_modes_passed and not test_failed_during_setup,
                'steps': step_results,
                'is_custom_mode': len(mode_results) == 1 and mode_results[0]['mode'] == 'custom',
                'is_example': is_example,
            })

        # Summary
        print(f"{Colors.BOLD}{'='*60}{Colors.RESET}")
        total = passed + failed
        pass_rate = (passed / total * 100) if total > 0 else 0

        if failed == 0:
            print(f"{Colors.GREEN}{Colors.BOLD}All tests passed! ({passed}/{total}){Colors.RESET}")
            success = True
        else:
            print(f"{Colors.RED}{passed} passed, {failed} failed ({pass_rate:.1f}%){Colors.RESET}")
            success = False

        # Always generate reports (HTML and text) for inspection
        if test_results:
            script_dir = Path(__file__).parent

            # Generate the full examples report (all tests)
            examples_full_path = script_dir / 'examples.html'
            generate_html_report(test_results, examples_full_path, examples_only=False)

            # Generate the filtered examples report (only tests marked as examples)
            examples_doc_path = script_dir / 'examples-doc.html'
            generate_html_report(test_results, examples_doc_path, examples_only=True)

            # Generate the examples snippet for embedding (only examples)
            examples_snippet_path = script_dir / 'examples-snippet.html'
            generate_examples_snippet(test_results, examples_snippet_path)

            # Generate the detailed test results report
            detailed_path = script_dir / 'test-results.html'
            generate_detailed_report(test_results, detailed_path)
            print(f"{Colors.BLUE}Detailed test results: {detailed_path}{Colors.RESET}")

            # Generate the text report
            text_path = script_dir / 'test-results.txt'
            generate_text_report(test_results, text_path)
            print(f"{Colors.BLUE}Text report: {text_path}{Colors.RESET}")

        # Write back the test file with replaced expectations if requested
        if replace_expected:
            # Write the entire YAML structure back (much simpler than line-by-line editing!)
            with open(test_file, 'w') as f:
                yaml.dump(data, f, default_flow_style=False, sort_keys=False, allow_unicode=True)
            print(f"\n{Colors.BLUE}Updated expected values in {test_file}{Colors.RESET}")

        return success


def main():
    """Main entry point"""
    import argparse

    parser = argparse.ArgumentParser(description='Run git-prompt tests')
    parser.add_argument('--verbose', '-v', action='store_true', help='Verbose output')
    parser.add_argument('--test-file', default='test_cases.yaml', help='Test cases YAML file')
    parser.add_argument('--replace-expected', action='store_true', help='Replace expected values with actual output (useful for updating tests after behavior changes)')

    args = parser.parse_args()

    # Resolve paths
    script_dir = Path(__file__).parent
    test_file = script_dir / args.test_file
    git_prompt_path = (script_dir / '../target/git-prompt').resolve()

    # Check files exist
    if not test_file.exists():
        print(f"{Colors.RED}Test file not found: {test_file}{Colors.RESET}")
        return 1

    if not git_prompt_path.exists():
        print(f"{Colors.RED}git-prompt not found: {git_prompt_path}{Colors.RESET}")
        print(f"Did you run 'make' in the parent directory?")
        return 1

    # Run tests (reports are always generated)
    success = run_test_suite(test_file, git_prompt_path, verbose=args.verbose, replace_expected=args.replace_expected)

    return 0 if success else 1


if __name__ == '__main__':
    sys.exit(main())
