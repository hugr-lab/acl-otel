import {themes as prismThemes} from 'prism-react-renderer';
import type {Config} from '@docusaurus/types';
import type * as Preset from '@docusaurus/preset-classic';

// Docs for acl-otel - OpenTelemetry for duckdb-acl's audit (and tresor's). Mirrors the
// tresor, mssql-extension and mssql-ducklake sites and the org site
// (hugr-lab.github.io) so they share look and feel; published by this repo's
// Pages workflow to https://hugr-lab.github.io/acl-otel/.

const config: Config = {
  title: 'acl-otel',
  tagline: 'OpenTelemetry for duckdb-acl - every access decision as a log record, a metric and a span, per role, per user, per door.',
  favicon: 'img/favicon.ico',

  url: 'https://hugr-lab.github.io',
  baseUrl: '/acl-otel/',
  trailingSlash: true,

  organizationName: 'hugr-lab',
  projectName: 'acl-otel',

  // 'throw', not 'warn': docs-build.yml is the PR gate for website/, and a
  // gate that exits 0 on a broken link does not gate anything.
  onBrokenLinks: 'throw',
  onBrokenAnchors: 'throw',

  i18n: {
    defaultLocale: 'en',
    locales: ['en'],
  },

  presets: [
    [
      'classic',
      {
        docs: {
          sidebarPath: './sidebars.ts',
          // Docs ARE the site: /acl-otel/<page>/
          routeBasePath: '/',
          // Versions are made at DEPLOY time from the release tags (pages.yml),
          // so nothing is committed for them; the live docs/ tree publishes as
          // "Next" once a release exists.
          editUrl: 'https://github.com/hugr-lab/acl-otel/tree/main/website/',
          showLastUpdateTime: true,
        },
        blog: false,
        theme: {
          customCss: './src/css/custom.css',
        },
      } satisfies Preset.Options,
    ],
  ],

  themes: ['@docusaurus/theme-mermaid'],

  markdown: {
    // the pages are plain Markdown full of `<role>` and `{"caps": ...}`: .md is CommonMark, never MDX
    format: 'detect',
    mermaid: true,
    hooks: {
      onBrokenMarkdownLinks: 'throw',
    },
  },

  themeConfig: {
    metadata: [
      {name: 'keywords', content: 'DuckDB, OpenTelemetry, OTLP, audit, access control, logs, metrics, traces, extension'},
      {name: 'description', content: 'DuckDB extension that exports duckdb-acl audit events, counters and execution profiles as OpenTelemetry logs, metrics and traces.'},
    ],
    navbar: {
      title: 'acl-otel',
      logo: {
        alt: 'Hugr Lab',
        src: 'img/logo-circle.svg',
        href: '/',
      },
      items: [
        {
          type: 'docSidebar',
          sidebarId: 'docsSidebar',
          position: 'left',
          label: 'Docs',
        },
        {
          to: '/reference/',
          label: 'Reference',
          position: 'left',
        },
        {
          href: 'https://hugr-lab.github.io/',
          label: 'Hugr Lab',
          position: 'right',
        },
        {
          href: 'https://github.com/hugr-lab/acl-otel',
          label: 'GitHub',
          position: 'right',
        },
      ],
    },
    colorMode: {
      defaultMode: 'light',
      disableSwitch: true,
      respectPrefersColorScheme: false,
    },
    footer: {
      style: 'dark',
      links: [
        {
          title: 'Docs',
          items: [
            {label: 'Getting started', to: '/getting-started/'},
            {label: 'Logs', to: '/logs/'},
            {label: 'Metrics', to: '/metrics/'},
            {label: 'Traces', to: '/traces/'},
          ],
        },
        {
          title: 'Community',
          items: [
            {label: 'GitHub', href: 'https://github.com/hugr-lab/acl-otel'},
            {label: 'Issues', href: 'https://github.com/hugr-lab/acl-otel/issues'},
            {label: 'duckdb-acl', href: 'https://hugr-lab.github.io/duckdb-acl/'},
          ],
        },
        {
          title: 'Hugr Lab',
          items: [
            {label: 'Main site', href: 'https://hugr-lab.github.io/'},
            {label: 'tresor', href: 'https://hugr-lab.github.io/tresor/'},
            {label: 'DuckDB MSSQL Extension', href: 'https://hugr-lab.github.io/mssql-extension/'},
          ],
        },
      ],
      copyright: `Copyright © ${new Date().getFullYear()} Hugr Lab.`,
    },
    prism: {
      theme: prismThemes.github,
      darkTheme: prismThemes.dracula,
      additionalLanguages: ['sql', 'bash', 'json', 'python', 'yaml'],
    },
  } satisfies Preset.ThemeConfig,
};

export default config;
