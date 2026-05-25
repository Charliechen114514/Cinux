import DefaultTheme from 'vitepress/theme'
import { h } from 'vue'
import type { Theme } from 'vitepress'
import HomeTipBanner from './components/HomeTipBanner.vue'
import HomeArchDiagram from './components/HomeArchDiagram.vue'
import DocNavCards from './components/DocNavCards.vue'
import projectConfig from '../../../project.config.ts'
import './custom.css'

export default {
  extends: DefaultTheme,
  Layout() {
    return h(DefaultTheme.Layout, null, {
      'home-features-before': () => h(HomeTipBanner, { config: projectConfig }),
      'home-features-after': () => h(HomeArchDiagram),
      'doc-after': () => h(DocNavCards)
    })
  },
} satisfies Theme
