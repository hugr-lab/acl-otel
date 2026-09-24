# Spec 012: the docs site

- **Status**: implemented
- **Date**: 2026-09-24
- **Mirrors**: duckdb-acl's site (its spec 087), and tresor's, mssql-extension's and mssql-ducklake's

## Problem

What the extension does, its settings, and how to wire it to a collector were spread over the README
and eleven specs. There was no page an operator could read start to finish.

## Design

`website/` is a Docusaurus 3 site with the same scaffold, look and workflows as duckdb-acl's:

- `markdown.format: 'detect'`, so `.md` is parsed as CommonMark;
- broken links and anchors fail the build;
- `docs-build.yml` builds the site on PRs;
- `pages.yml` deploys `main` to https://hugr-lab.github.io/acl-otel/, with a docs version per release
  tag.

The pages are written from the specs and checked against the code: attribute and metric names,
severities, the rules table's columns, the allowlist's JSON shape.

| page | content |
| --- | --- |
| `index.md` | what the extension is and how it attaches |
| `getting-started.md` | load, endpoint, see a record |
| `levels-and-rules.md` | the level rules, the central table, sampling (specs 001, 005, 007) |
| `logs.md` | the record (specs 002, 005) |
| `metrics.md` | acl's numbers, the histograms, the named series, health (specs 003, 006) |
| `traces.md` | decision spans and execution profiles (specs 008, 009) |
| `tresor.md` | tresor's audit (spec 011) |
| `reference.md` | the functions and settings tables, taken from the README |
| `deployment.md` | the OpenTelemetry environment, Application Insights, the local stack, the readiness probe |
| `development.md` | build, test, specs |

## Tests

`npx docusaurus build` and `check_docs_links.py` pass.

## Follow-ups

- **Pages.** GitHub Pages must be enabled for the repository (source: GitHub Actions) before the first
  deploy. That is the owner's switch.
- **The reference.** It copies the README's tables. Once the site is live, the README could link to
  it instead of repeating them.
