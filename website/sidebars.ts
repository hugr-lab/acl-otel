import type {SidebarsConfig} from '@docusaurus/plugin-content-docs';

const sidebars: SidebarsConfig = {
  docsSidebar: [
    'index',
    'getting-started',
    'levels-and-rules',
    {
      type: 'category',
      label: 'Signals',
      collapsed: false,
      items: ['logs', 'metrics', 'traces', 'tresor'],
    },
    'reference',
    'deployment',
    'development',
  ],
};

export default sidebars;
