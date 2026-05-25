import { defineProject } from './site/.vitepress/config/schema'

export default defineProject({
  name: 'cinux',
  title: { 'zh-CN': 'Cinux' },
  description: { 'zh-CN': '64位OS教程' },
  base: '/cinux/',
  copyright: 'Copyright © 2026 Charliechen - 保留所有权利',

  documentsDir: 'document',
  siteDir: 'site',

  locales: [
    { code: 'zh-CN', label: '中文', default: true },
  ],

  nav: {
    'zh-CN': [
      { text: '首页', link: '/' },
      { text: 'Hands-on', link: '/hands-on/' },
      { text: 'Read-through', link: '/read-through/' },
      { text: 'Tutorial', link: '/tutorial/' },
      { text: '笔记', link: '/notes/' },
      { text: 'CI', link: '/ci/' },
      { text: 'GitHub', link: 'https://github.com/Charliechen114514/cinux' },
    ],
  },

  sidebar: {
    volumes: [
      { name: 'hands-on', srcDir: 'hands-on', urlPrefix: '/hands-on' },
      { name: 'read-through', srcDir: 'read-through', urlPrefix: '/read-through' },
      { name: 'tutorial', srcDir: 'tutorial', urlPrefix: '/tutorial' },
      { name: 'notes', srcDir: 'notes', urlPrefix: '/notes' },
      { name: 'ci', srcDir: 'ci', urlPrefix: '/ci' },
      { name: 'document', srcDir: 'document', urlPrefix: '/document' },
    ],
  },

  github: {
    owner: 'Charliechen114514',
    repo: 'cinux',
    branch: 'main',
    documentsPath: 'document',
  },

  build: {
    concurrency: 4,
    rootPages: ['index.md'],
    rootAssets: [],
  },

  plugins: {
    cppTemplateEscape: true,
    kbd: true,
    math: true,
  },

  homeBanner: {
    'zh-CN': '🚀 新手必读：从环境搭建开始，请查看 <a href="/cinux/hands-on/">Hands-on 教程</a>，了解如何构建你的第一个 x86_64 内核。',
  },
})
