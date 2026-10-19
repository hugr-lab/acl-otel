import {themes as prismThemes} from 'prism-react-renderer';
import type {Config} from '@docusaurus/types';
import type * as Preset from '@docusaurus/preset-classic';

// Docs for acl-otel - OpenTelemetry for duckdb-acl's audit (and tresor's). Mirrors the
// tresor, mssql-extension and mssql-ducklake sites and the org site
// (hugr-lab.github.io) so they share look and feel; published by this repo's
// Pages workflow to https://hugr-lab.github.io/acl-otel/ and, as a second build,
// to https://hugr-lab.com/docs/acl-otel/ (IONOS) - that copy loads nothing from
// another host (the site's CSP and its Datenschutz say so).

// The same docs are built twice: for GitHub Pages (default) and for hugr-lab.com on IONOS (DOCS_TARGET=ionos).
const ionos = process.env.DOCS_TARGET === 'ionos';
// While the hugr-lab.com copy exists (DOCS_IONOS on), pages.yml sets DOCS_CANONICAL for
// the GitHub Pages build: its pages then name the hugr-lab.com copy as canonical
// (src/theme/SiteMetadata) and it has no sitemap of its own.
const canonicalBase = ionos ? '' : process.env.DOCS_CANONICAL ?? '';
const site = ionos
  ? {url: 'https://hugr-lab.com', baseUrl: '/docs/acl-otel/', home: 'https://hugr-lab.com/', impressum: 'https://hugr-lab.com/impressum/', datenschutz: 'https://hugr-lab.com/datenschutz/'}
  : {url: 'https://hugr-lab.github.io', baseUrl: '/acl-otel/', home: 'https://hugr-lab.github.io/', impressum: 'https://hugr-lab.github.io/impressum/', datenschutz: 'https://hugr-lab.github.io/datenschutz/'};

const config: Config = {
  title: 'acl-otel',
  tagline: 'OpenTelemetry for duckdb-acl - every access decision as a log record, a metric and a span, per role, per user, per door.',
  favicon: 'img/favicon.ico',

  url: site.url,
  baseUrl: site.baseUrl,
  baseUrlIssueBanner: false,
  trailingSlash: true,
  customFields: {docsTarget: ionos ? 'ionos' : 'github', canonicalBase},

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
        sitemap: canonicalBase ? false : undefined,
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
    // the hugr-lab.com copy requests no other server: an external image (a badge) is dropped from every page and
    // every version snapshotted from a tag - those are never edited, so this happens at build time
    preprocessor: ({fileContent}) =>
      ionos
        ? fileContent.replace(/\[!\[[^\]]*\]\(https?:\/\/[^)]*\)\]\([^)]*\)|!\[[^\]]*\]\(https?:\/\/[^)]*\)/g, '')
        : fileContent,
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
          href: site.home,
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
            {label: 'Main site', href: site.home},
            {label: 'tresor', href: 'https://hugr-lab.github.io/tresor/'},
            {label: 'DuckDB MSSQL Extension', href: 'https://hugr-lab.github.io/mssql-extension/'},
          ],
        },
        {
          title: 'Legal',
          items: [
            {label: 'Impressum', href: site.impressum},
            {label: 'Datenschutz', href: site.datenschutz},
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
