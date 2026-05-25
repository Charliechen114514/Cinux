<script setup lang="ts">
const layers = [
  {
    icon: '👤',
    label: '用户空间',
    sublabel: 'User Space',
    items: ['C++ 应用', 'Shell (CFBox)', '系统调用接口']
  },
  {
    icon: '📚',
    label: 'C++ 标准库',
    sublabel: 'libc++',
    items: ['libcxxrt', 'freestanding 头', 'glibc 兼容层']
  },
  {
    icon: '🔧',
    label: '内核',
    sublabel: 'Kernel',
    items: ['进程调度', '虚拟内存', 'VFS 文件系统', '设备驱动'],
    extras: ['x86_64 特有: GDT/IDT/PCI/APIC']
  },
  {
    icon: '🚀',
    label: 'Bootloader',
    sublabel: 'Multiboot2',
    flow: 'GRUB → 加载内核 ELF → 进入 Long Mode → 跳转 kernel_main'
  },
  {
    icon: '💻',
    label: 'x86_64 硬件',
    sublabel: 'Hardware',
    items: ['QEMU / Bochs', 'VGA 文本模式', '键盘驱动', '磁盘 I/O']
  }
]
</script>

<template>
  <div class="home-arch">
    <div class="home-arch-inner">
      <div class="home-arch-header">
        <h2 class="home-arch-title">系统架构概览</h2>
        <p class="home-arch-desc">从硬件到用户空间，理解 x86_64 操作系统各层级的协作关系</p>
      </div>

      <div class="arch-diagram">
        <div class="arch-layers">
          <div
            v-for="(layer, idx) in layers"
            :key="idx"
            class="arch-layer"
            :class="`arch-layer--${idx}`"
          >
            <div class="arch-layer-left">
              <span class="arch-layer-icon">{{ layer.icon }}</span>
              <div class="arch-layer-info">
                <span class="arch-layer-label">{{ layer.label }}</span>
                <span class="arch-layer-sub">{{ layer.sublabel }}</span>
              </div>
            </div>
            <div class="arch-layer-right">
              <div v-if="layer.items" class="arch-tags">
                <span v-for="t in layer.items" :key="t" class="arch-tag">{{ t }}</span>
              </div>
              <div v-if="layer.extras" class="arch-tags">
                <span v-for="t in layer.extras" :key="t" class="arch-tag arch-tag--ghost">{{ t }}</span>
              </div>
              <div v-if="layer.flow" class="arch-flow">{{ layer.flow }}</div>
            </div>
          </div>
        </div>

        <div class="arch-boot-axis">
          <span class="arch-boot-label">Boot Flow</span>
          <div class="arch-boot-line">
            <div class="arch-boot-dots">
              <span v-for="i in 5" :key="i" class="arch-boot-dot" />
            </div>
            <svg class="arch-boot-arrow" width="14" height="10" viewBox="0 0 14 10">
              <path d="M1 1 L7 8 L13 1" stroke="currentColor" stroke-width="1.5" fill="none" stroke-linecap="round" stroke-linejoin="round" />
            </svg>
          </div>
        </div>
      </div>

      <div class="home-arch-cta">
        <a href="/cinux/hands-on/" class="arch-cta">
          <span>开始动手实践</span>
          <span class="arch-cta-arrow">→</span>
        </a>
      </div>
    </div>
  </div>
</template>

<style scoped>
.home-arch {
  padding: 64px 24px 56px;
  overflow: visible;
}

.home-arch-inner {
  max-width: 920px;
  margin: 0 auto;
}

/* ── Header ── */
.home-arch-header {
  text-align: center;
  margin-bottom: 44px;
}

.home-arch-title {
  margin: 0;
  font-size: 28px;
  font-weight: 700;
  line-height: 1.5;
  background: linear-gradient(135deg, var(--vp-c-brand-1), var(--vp-c-indigo-1), var(--vp-c-purple-1));
  -webkit-background-clip: text;
  background-clip: text;
  -webkit-text-fill-color: transparent;
}

.home-arch-desc {
  margin: 10px 0 0;
  font-size: 15px;
  color: var(--vp-c-text-2);
  line-height: 1.7;
}

/* ── Diagram container ── */
.arch-diagram {
  display: flex;
  gap: 20px;
}

.arch-layers {
  flex: 1;
  min-width: 0;
  border-radius: 20px;
  overflow: hidden;
  border: 1px solid var(--vp-c-divider);
  box-shadow: 0 2px 8px rgba(0, 0, 0, 0.04);
}

.dark .arch-layers {
  border-color: var(--vp-c-border);
  box-shadow: 0 2px 8px rgba(0, 0, 0, 0.15);
}

/* ── Single layer band ── */
.arch-layer {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 24px;
  padding: 28px 32px;
  transition: background 0.35s ease;
}

.arch-layer:not(:last-child) {
  border-bottom: 1px solid var(--vp-c-divider);
}

.dark .arch-layer:not(:last-child) {
  border-bottom-color: var(--vp-c-border);
}

/* Per-layer tint */
.arch-layer--0 { background: rgba(139, 92, 246, 0.04); }
.arch-layer--1 { background: rgba(99, 102, 241, 0.04); }
.arch-layer--2 { background: rgba(81, 107, 232, 0.04); }
.arch-layer--3 { background: rgba(34, 197, 94, 0.04); }
.arch-layer--4 { background: rgba(251, 146, 60, 0.04); }

.arch-layer--0:hover { background: rgba(139, 92, 246, 0.09); }
.arch-layer--1:hover { background: rgba(99, 102, 241, 0.09); }
.arch-layer--2:hover { background: rgba(81, 107, 232, 0.09); }
.arch-layer--3:hover { background: rgba(34, 197, 94, 0.09); }
.arch-layer--4:hover { background: rgba(251, 146, 60, 0.09); }

.dark .arch-layer--0 { background: rgba(139, 92, 246, 0.06); }
.dark .arch-layer--1 { background: rgba(99, 102, 241, 0.06); }
.dark .arch-layer--2 { background: rgba(81, 107, 232, 0.06); }
.dark .arch-layer--3 { background: rgba(34, 197, 94, 0.06); }
.dark .arch-layer--4 { background: rgba(251, 146, 60, 0.06); }

.dark .arch-layer--0:hover { background: rgba(139, 92, 246, 0.12); }
.dark .arch-layer--1:hover { background: rgba(99, 102, 241, 0.12); }
.dark .arch-layer--2:hover { background: rgba(81, 107, 232, 0.12); }
.dark .arch-layer--3:hover { background: rgba(34, 197, 94, 0.12); }
.dark .arch-layer--4:hover { background: rgba(251, 146, 60, 0.12); }

/* ── Layer left (icon + label) ── */
.arch-layer-left {
  display: flex;
  align-items: center;
  gap: 14px;
  flex-shrink: 0;
  min-width: 160px;
}

.arch-layer-icon {
  font-size: 26px;
  line-height: 1;
}

.arch-layer-info {
  display: flex;
  flex-direction: column;
  gap: 1px;
}

.arch-layer-label {
  font-size: 16px;
  font-weight: 600;
  color: var(--vp-c-text-1);
  line-height: 1.35;
}

.arch-layer-sub {
  font-size: 12px;
  color: var(--vp-c-text-3);
}

/* ── Layer right (tags / flow) ── */
.arch-layer-right {
  display: flex;
  flex-direction: column;
  gap: 6px;
  align-items: flex-end;
}

.arch-tags {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
  justify-content: flex-end;
}

.arch-tag {
  display: inline-flex;
  align-items: center;
  padding: 4px 12px;
  border-radius: 8px;
  font-size: 12.5px;
  font-weight: 500;
  background: var(--vp-c-brand-soft);
  color: var(--vp-c-brand-1);
  white-space: nowrap;
}

.arch-tag--ghost {
  background: rgba(81, 107, 232, 0.06);
  color: var(--vp-c-text-2);
}

.dark .arch-tag--ghost {
  background: rgba(81, 107, 232, 0.1);
}

.arch-flow {
  font-size: 13px;
  color: var(--vp-c-text-3);
  letter-spacing: 0.03em;
}

/* ── Boot flow axis (right side) ── */
.arch-boot-axis {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 10px;
  padding-top: 28px;
  padding-bottom: 28px;
}

.arch-boot-label {
  font-size: 11px;
  font-weight: 600;
  text-transform: uppercase;
  letter-spacing: 0.1em;
  color: var(--vp-c-text-3);
  writing-mode: vertical-rl;
  text-orientation: mixed;
  transform: rotate(180deg);
}

.arch-boot-line {
  flex: 1;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: space-between;
}

.arch-boot-dots {
  display: flex;
  flex-direction: column;
  gap: 28px;
  padding: 4px 0;
}

.arch-boot-dot {
  width: 7px;
  height: 7px;
  border-radius: 50%;
  background: var(--vp-c-brand-1);
  opacity: 0.5;
}

.arch-boot-arrow {
  color: var(--vp-c-brand-1);
  opacity: 0.6;
}

/* ── CTA ── */
.home-arch-cta {
  display: flex;
  justify-content: center;
  margin-top: 48px;
}

.arch-cta {
  display: inline-flex;
  align-items: center;
  gap: 10px;
  padding: 14px 32px;
  border-radius: 14px;
  background: linear-gradient(135deg, var(--vp-c-brand-1), var(--vp-c-indigo-1));
  color: var(--vp-c-white);
  font-size: 16px;
  font-weight: 600;
  text-decoration: none !important;
  box-shadow: 0 4px 16px rgba(81, 107, 232, 0.25);
  transition: transform 0.35s ease, box-shadow 0.35s ease;
}

.arch-cta:hover {
  transform: translateY(-2px);
  box-shadow: 0 8px 28px rgba(81, 107, 232, 0.35);
}

.arch-cta-arrow {
  transition: transform 0.35s ease;
}

.arch-cta:hover .arch-cta-arrow {
  transform: translateX(4px);
}

/* ── Responsive ── */
@media (max-width: 768px) {
  .arch-diagram {
    flex-direction: column;
    gap: 0;
  }

  .arch-boot-axis {
    flex-direction: row;
    padding: 16px 0 0;
    gap: 12px;
  }

  .arch-boot-label {
    writing-mode: horizontal-tb;
    transform: none;
  }

  .arch-boot-dots {
    flex-direction: row;
    gap: 24px;
  }

  .arch-boot-arrow {
    transform: rotate(90deg);
  }

  .arch-layer {
    flex-direction: column;
    align-items: flex-start;
    gap: 12px;
    padding: 22px 24px;
  }

  .arch-layer-right {
    align-items: flex-start;
  }

  .arch-tags {
    justify-content: flex-start;
  }
}

@media (max-width: 639px) {
  .home-arch {
    padding: 40px 16px 36px;
  }

  .home-arch-title {
    font-size: 22px;
  }

  .arch-layer {
    padding: 18px 18px;
  }

  .arch-layer-left {
    min-width: unset;
  }

  .arch-layer-icon {
    font-size: 22px;
  }

  .arch-layer-label {
    font-size: 15px;
  }

  .arch-tag {
    font-size: 12px;
    padding: 3px 10px;
  }

  .home-arch-cta {
    margin-top: 36px;
  }

  .arch-cta {
    padding: 12px 24px;
    font-size: 15px;
  }
}
</style>
