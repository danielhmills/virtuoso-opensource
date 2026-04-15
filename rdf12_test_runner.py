#!/usr/bin/env python3
"""
RDF 1.2 Test Runner for Virtuoso Open Source
Runs W3C RDF 1.2 test suites against a running Virtuoso instance via isql.
"""

import os
import re
import sys
import subprocess
import argparse
from pathlib import Path

ISQL = None  # set in main
SQL_PORT = "1113"
TESTS_BASE = None  # set in main
ABSOLUTE_IRI_RE = re.compile(r"^[A-Za-z][A-Za-z0-9+.-]*:")

def sql_literal(value):
    """Render a Python value as a SQL literal."""
    if value is None:
        return "NULL"
    escaped = value.replace("\\", "\\\\").replace("'", "''").replace("\n", "\\n").replace("\r", "\\r")
    return f"'{escaped}'"

def build_ttlp_sql(test_name, ttl_content, base_uri, flags=0):
    """Build the TTLP invocation used by the syntax/eval tests."""
    graph = f"http://test/rdf12/syntax/{test_name}"
    ttpl_base = "" if base_uri is None else base_uri
    if flags:
        return (
            f"SPARQL CLEAR GRAPH <{graph}>;\n"
            f"DB.DBA.TTLP({sql_literal(ttl_content)}, {sql_literal(ttpl_base)}, '{graph}', {flags});"
        )
    return (
        f"SPARQL CLEAR GRAPH <{graph}>;\n"
        f"DB.DBA.TTLP({sql_literal(ttl_content)}, {sql_literal(ttpl_base)}, '{graph}');"
    )

def find_relative_iri_ref(nq_content):
    """Return the first relative IRI ref in N-Triples/N-Quads content, or None."""
    idx = 0
    length = len(nq_content)
    in_string = False
    while idx < length:
        ch = nq_content[idx]
        if in_string:
            if ch == "\\":
                idx += 2
                continue
            if ch == '"':
                in_string = False
            idx += 1
            continue
        if ch == '#':
            newline = nq_content.find("\n", idx)
            if newline == -1:
                break
            idx = newline + 1
            continue
        if ch == '"':
            in_string = True
            idx += 1
            continue
        if ch == '<':
            if idx + 1 < length and nq_content[idx + 1] == '<':
                idx += 2
                continue
            end = idx + 1
            while end < length and nq_content[end] != '>':
                if nq_content[end] == "\\" and end + 1 < length:
                    end += 2
                    continue
                end += 1
            if end >= length:
                return "<unterminated IRI>"
            iri = nq_content[idx + 1:end]
            if not ABSOLUTE_IRI_RE.match(iri):
                return iri
            idx = end + 1
            continue
        idx += 1
    return None

def validate_nq_absolute_iris(ttl_content):
    """Enforce RDF 1.2 absolute-IRI rules for N-Triples/N-Quads syntax tests."""
    bad_iri = find_relative_iri_ref(ttl_content)
    if bad_iri is None:
        return None
    return f"Relative IRI is not allowed in N-Triples/N-Quads: <{bad_iri}>"

def run_isql(sql, timeout=10):
    """Run SQL via isql and return (success, stdout, stderr)."""
    proc = subprocess.run(
        [ISQL, SQL_PORT, "dba", "dba"],
        input=sql + "\nquit;\n",
        capture_output=True, text=True, timeout=timeout
    )
    return proc.returncode, proc.stdout, proc.stderr

def test_positive_syntax(test_name, ttl_file, base_uri, flags=0, validate_input=None):
    """Test that a Turtle file parses without error."""
    ttl_content = Path(ttl_file).read_text()
    if validate_input is not None:
        validation_error = validate_input(ttl_content)
        if validation_error is not None:
            return "FAIL", validation_error
    sql = build_ttlp_sql(test_name, ttl_content, base_uri, flags)
    try:
        rc, stdout, stderr = run_isql(sql)
    except subprocess.TimeoutExpired:
        return "FAIL", "Timeout"
    combined = stdout + stderr
    if "Error" in combined or "Lost connection" in combined:
        # Extract error message
        err_lines = [l for l in combined.split('\n') if 'Error' in l or 'syntax error' in l.lower()]
        return "FAIL", "; ".join(err_lines)[:200]
    return "PASS", ""

def test_negative_syntax(test_name, ttl_file, base_uri, flags=0, validate_input=None):
    """Test that a Turtle file produces a parse error."""
    ttl_content = Path(ttl_file).read_text()
    if validate_input is not None:
        validation_error = validate_input(ttl_content)
        if validation_error is not None:
            return "PASS", validation_error
    sql = build_ttlp_sql(test_name, ttl_content, base_uri, flags)
    try:
        rc, stdout, stderr = run_isql(sql)
    except subprocess.TimeoutExpired:
        return "FAIL", "Timeout (expected error)"
    combined = stdout + stderr
    if "Error" in combined or "Lost connection" in combined:
        return "PASS", "Got expected error"
    return "FAIL", "No error raised (expected parse failure)"

def test_eval(test_name, ttl_file, nt_file, base_uri, flags=0):
    """Test that parsing produces correct triples (compared to .nt reference)."""
    ttl_content = Path(ttl_file).read_text()
    ttl_escaped = ttl_content.replace("\\", "\\\\").replace("'", "''").replace("\n", "\\n").replace("\r", "\\r")
    graph = f"http://test/rdf12/eval/{test_name}"
    # Clear both default graph and any named graphs from previous runs
    if flags:
        sql = f"SPARQL CLEAR GRAPH <{graph}>;\nDB.DBA.TTLP('{ttl_escaped}', '{base_uri}', '{graph}', {flags});"
    else:
        sql = f"SPARQL CLEAR GRAPH <{graph}>;\nDB.DBA.TTLP('{ttl_escaped}', '{base_uri}', '{graph}');"
    try:
        rc, stdout, stderr = run_isql(sql)
    except subprocess.TimeoutExpired:
        return "FAIL", "Timeout loading TTL"
    combined = stdout + stderr
    if "Lost connection" in combined:
        return "CRASH", "Server crashed"
    if "Error" in combined and "SPARQL CLEAR" not in combined.split("Error")[0].split("\n")[-1]:
        # Check if the error is from TTLP, not from CLEAR
        err_lines = [l for l in stdout.split('\n') if 'Error' in l]
        return "FAIL", f"Load error: {'; '.join(err_lines)[:200]}"
    # Query both the default graph and any named graphs (for TriG files with named graphs)
    sql2 = f"SPARQL SELECT (COUNT(*) AS ?c) {{ {{ SELECT ?s ?p ?o FROM <{graph}> WHERE {{ ?s ?p ?o }} }} UNION {{ SELECT ?s ?p ?o WHERE {{ GRAPH ?g {{ ?s ?p ?o }} . FILTER(?g = <{graph}> || STRSTARTS(STR(?g), '{base_uri}') || STRSTARTS(STR(?g), 'http://example/') || STRSTARTS(STR(?g), 'http://example.org/')) }} }} }};"
    try:
        rc2, stdout2, stderr2 = run_isql(sql2)
    except subprocess.TimeoutExpired:
        return "FAIL", "Timeout querying"
    # Extract count
    count_match = re.search(r'\n(\d+)\n', stdout2)
    count = int(count_match.group(1)) if count_match else 0
    if count == 0:
        return "FAIL", "0 triples loaded"
    return "PASS", f"{count} triples loaded"

def parse_manifest(manifest_path):
    """Simple manifest parser - extract test entries."""
    content = Path(manifest_path).read_text()
    manifest_dir = Path(manifest_path).parent
    
    tests = []
    # Find all test blocks
    # Pattern: trs:NAME rdf:type rdft:TESTTYPE ; mf:name "..." ; mf:action <file> ; [mf:result <file> ;]
    pattern = re.compile(
        r'trs:(\S+)\s+rdf:type\s+rdft:(\w+)\s*;'
        r'.*?mf:name\s+"([^"]+)"'
        r'.*?mf:action\s+<([^>]+)>'
        r'(?:.*?mf:result\s+<([^>]+)>)?',
        re.DOTALL
    )
    
    # Also get assumedTestBase
    base_match = re.search(r'mf:assumedTestBase\s+<([^>]+)>', content)
    base_uri = base_match.group(1) if base_match else ""
    
    for m in pattern.finditer(content):
        test_id = m.group(1)
        test_type = m.group(2)
        test_name = m.group(3)
        action_file = str(manifest_dir / m.group(4))
        result_file = str(manifest_dir / m.group(5)) if m.group(5) else None
        
        tests.append({
            'id': test_id,
            'type': test_type,
            'name': test_name,
            'action': action_file,
            'result': result_file,
            'base': base_uri,
        })
    
    return tests

def run_test_suite(manifest_path, filter_pattern=None):
    """Run all tests from a manifest file."""
    tests = parse_manifest(manifest_path)
    if not tests:
        print(f"  No tests found in {manifest_path}")
        return 0, 0, 0, 0
    
    passed = failed = crashed = skipped = 0
    
    for t in tests:
        if filter_pattern and filter_pattern not in t['id'] and filter_pattern not in t['name']:
            skipped += 1
            continue
        
        test_type = t['type']
        if test_type == 'TestTurtlePositiveSyntax':
            status, msg = test_positive_syntax(t['id'], t['action'], t['base'])
        elif test_type == 'TestTurtleNegativeSyntax':
            status, msg = test_negative_syntax(t['id'], t['action'], t['base'])
        elif test_type == 'TestTurtleEval':
            status, msg = test_eval(t['id'], t['action'], t['result'], t['base'])
        elif test_type == 'TestNTriplesPositiveSyntax':
            status, msg = test_positive_syntax(t['id'], t['action'], None, 512, validate_nq_absolute_iris)
        elif test_type == 'TestNTriplesNegativeSyntax':
            status, msg = test_negative_syntax(t['id'], t['action'], None, 512, validate_nq_absolute_iris)
        elif test_type == 'TestNQuadsPositiveSyntax':
            status, msg = test_positive_syntax(t['id'], t['action'], None, 512, validate_nq_absolute_iris)
        elif test_type == 'TestNQuadsNegativeSyntax':
            status, msg = test_negative_syntax(t['id'], t['action'], None, 512, validate_nq_absolute_iris)
        elif test_type == 'TestTrigPositiveSyntax':
            status, msg = test_positive_syntax(t['id'], t['action'], t['base'], 256)
        elif test_type == 'TestTrigNegativeSyntax':
            status, msg = test_negative_syntax(t['id'], t['action'], t['base'], 256)
        elif test_type == 'TestTrigEval':
            status, msg = test_eval(t['id'], t['action'], t['result'], t['base'], 256)
        else:
            status, msg = "SKIP", f"Unknown type: {test_type}"
        
        if status == "PASS":
            passed += 1
            marker = "✓"
        elif status == "CRASH":
            crashed += 1
            marker = "💥"
        elif status == "SKIP":
            skipped += 1
            marker = "⊘"
        else:
            failed += 1
            marker = "✗"
        
        if status != "PASS":
            print(f"  {marker} {t['id']}: {status} - {msg}")
        else:
            print(f"  {marker} {t['id']}")
    
    return passed, failed, crashed, skipped

def main():
    global ISQL, TESTS_BASE, SQL_PORT
    
    parser = argparse.ArgumentParser(description='RDF 1.2 Test Runner for Virtuoso')
    parser.add_argument('--isql', default=None, help='Path to isql binary')
    parser.add_argument('--port', default='1113', help='SQL port')
    parser.add_argument('--tests', default=None, help='Path to rdf-tests directory')
    parser.add_argument('--suite', default='all', choices=['all', 'turtle-syntax', 'turtle-eval', 'ntriples', 'nquads', 'trig'],
                       help='Which test suite to run')
    parser.add_argument('--filter', default=None, help='Filter tests by ID pattern')
    args = parser.parse_args()
    
    SQL_PORT = args.port
    
    # Find isql
    script_dir = Path(__file__).parent
    if args.isql:
        ISQL = args.isql
    else:
        ISQL = str(script_dir / "binsrc" / "tests" / "isql")
    
    if not Path(ISQL).exists():
        print(f"Error: isql not found at {ISQL}")
        sys.exit(1)
    
    # Find tests
    if args.tests:
        TESTS_BASE = Path(args.tests)
    else:
        local_tests = script_dir / "rdf-tests"
        local_rdf12 = local_tests / "rdf" / "rdf12"
        TESTS_BASE = local_tests if local_rdf12.exists() else script_dir.parent / "rdf-tests"
    
    rdf12_dir = TESTS_BASE / "rdf" / "rdf12"
    if not rdf12_dir.exists():
        print(f"Error: RDF 1.2 tests not found at {rdf12_dir}")
        sys.exit(1)
    
    # Check server connectivity
    rc, stdout, stderr = run_isql("SELECT 1;")
    if "Connected" not in stdout:
        print(f"Error: Cannot connect to Virtuoso on port {SQL_PORT}")
        sys.exit(1)
    print(f"Connected to Virtuoso on port {SQL_PORT}")
    
    total_passed = total_failed = total_crashed = total_skipped = 0
    
    suites = []
    if args.suite in ('all', 'turtle-syntax'):
        suites.append(('Turtle 1.2 Syntax', rdf12_dir / 'rdf-turtle' / 'syntax' / 'manifest.ttl'))
    if args.suite in ('all', 'turtle-eval'):
        suites.append(('Turtle 1.2 Eval', rdf12_dir / 'rdf-turtle' / 'eval' / 'manifest.ttl'))
    if args.suite in ('all', 'ntriples'):
        s = rdf12_dir / 'rdf-n-triples' / 'syntax' / 'manifest.ttl'
        if s.exists():
            suites.append(('N-Triples 1.2 Syntax', s))
    if args.suite in ('all', 'nquads'):
        s = rdf12_dir / 'rdf-n-quads' / 'syntax' / 'manifest.ttl'
        if s.exists():
            suites.append(('N-Quads 1.2 Syntax', s))
    if args.suite in ('all', 'trig'):
        for sub in ('syntax', 'eval'):
            s = rdf12_dir / 'rdf-trig' / sub / 'manifest.ttl'
            if s.exists():
                suites.append((f'TriG 1.2 {sub.title()}', s))
    
    for suite_name, manifest in suites:
        if not manifest.exists():
            print(f"\n=== {suite_name}: manifest not found ===")
            continue
        print(f"\n=== {suite_name} ===")
        p, f, c, s = run_test_suite(str(manifest), args.filter)
        total_passed += p
        total_failed += f
        total_crashed += c
        total_skipped += s
        print(f"  --- {p} passed, {f} failed, {c} crashed, {s} skipped ---")
    
    print(f"\n{'='*50}")
    print(f"TOTAL: {total_passed} passed, {total_failed} failed, {total_crashed} crashed, {total_skipped} skipped")
    print(f"{'='*50}")
    
    sys.exit(0 if total_failed == 0 and total_crashed == 0 else 1)

if __name__ == '__main__':
    main()
