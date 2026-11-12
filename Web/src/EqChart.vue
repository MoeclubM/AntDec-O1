<script setup lang="ts">
import { computed, ref } from 'vue';
import type { Band } from './transports';
import { frequencyAt, gainAt, graphX, graphY, responsePath } from './eq';

const props = defineProps<{ bands: Band[]; preamp: number; rate: number; caption: string;
  disabled: boolean; valid: boolean }>();
const selected = defineModel<number>('selected', { default: -1 });
const emit = defineEmits<{ add: [frequency: number, gain: number];
  change: [index: number, values: Partial<Band>]; remove: [index: number] }>();
const svg = ref<SVGSVGElement>();
const points = computed(() => props.bands.map((band, index) => ({ band, index }))
  .filter(p => p.band.type !== 0 && Number.isFinite(p.band.frequency) && Number.isFinite(p.band.gain)));
const path = computed(() => props.valid ? responsePath(props.bands, props.preamp, props.rate) : '');
const frequencyTicks = [20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000, 40000];
let dragging: number | undefined;

function position(event: PointerEvent | MouseEvent) {
  const point = svg.value!.createSVGPoint();
  point.x = event.clientX; point.y = event.clientY;
  return point.matrixTransform(svg.value!.getScreenCTM()!.inverse());
}
function add(event: MouseEvent) {
  if (props.disabled || (event.target as Element).closest('.eq-node')) return;
  const point = position(event);
  if (point.x < 42 || point.x > 932 || point.y < 21 || point.y > 189) return;
  emit('add', Math.round(frequencyAt(point.x)), Math.round(gainAt(point.y) * 10) / 10);
}
function start(event: PointerEvent, index: number) {
  if (props.disabled) return;
  event.preventDefault();
  selected.value = index; dragging = index;
  (event.currentTarget as SVGElement).focus();
  svg.value!.setPointerCapture(event.pointerId);
}
function move(event: PointerEvent) {
  if (dragging === undefined || props.disabled) return;
  const point = position(event), band = props.bands[dragging];
  emit('change', dragging, { frequency: Math.round(frequencyAt(point.x)),
    gain: band.type >= 4 ? band.gain : Math.round(gainAt(point.y) * 10) / 10 });
}
function finish(event: PointerEvent) {
  dragging = undefined;
  if (svg.value?.hasPointerCapture(event.pointerId)) svg.value.releasePointerCapture(event.pointerId);
}
function key(event: KeyboardEvent, index: number) {
  if (props.disabled) return;
  const band = props.bands[index];
  if (event.key === 'Delete' || event.key === 'Backspace') {
    event.preventDefault(); emit('remove', index); return;
  }
  if (!event.key.startsWith('Arrow')) return;
  event.preventDefault();
  if (event.key === 'ArrowLeft' || event.key === 'ArrowRight') {
    const frequency = Math.round(band.frequency * 2 ** ((event.key === 'ArrowRight' ? 1 : -1) / (event.shiftKey ? 120 : 24)));
    emit('change', index, { frequency: Math.max(20, Math.min(40000, frequency)) });
  } else if (band.type < 4) {
    const gain = Math.round((band.gain + (event.key === 'ArrowUp' ? 1 : -1) * (event.shiftKey ? 1 : .1)) * 10) / 10;
    emit('change', index, { gain: Math.max(-18, Math.min(18, gain)) });
  }
}
</script>

<template>
  <div class="response-chart">
    <div class="chart-caption"><span>频率响应</span><span>{{ caption }} · {{ rate / 1000 }} kHz</span></div>
    <svg ref="svg" viewBox="0 0 960 224" role="group" aria-label="EQ 频率响应与频段控制点"
      :class="{ editable: !disabled }" @dblclick="add" @pointermove="move" @pointerup="finish"
      @pointercancel="finish" @lostpointercapture="dragging=undefined">
      <g v-for="db in [-24,-12,0,12,24]" :key="db">
        <line x1="42" x2="932" :y1="graphY(db)" :y2="graphY(db)" :class="db===0?'zero':'grid'"/>
        <text x="32" :y="graphY(db)+4" text-anchor="end">{{ db>0?'+'+db:db }}</text>
      </g>
      <g v-for="f in frequencyTicks" :key="f">
        <line :x1="graphX(f)" :x2="graphX(f)" y1="21" y2="189" class="grid"/>
        <text :x="graphX(f)" y="213" text-anchor="middle">{{ f>=1000?f/1000+'k':f }}</text>
      </g>
      <path :d="path" class="response"/>
      <g v-for="{band,index} in points" :key="index" class="eq-node"
        :class="{selected:selected===index, inactive:!band.enabled || band.frequency>=rate*.47}">
        <circle :cx="graphX(band.frequency)" :cy="graphY(band.type>=4?0:band.gain)" r="10" class="node-target"
          role="button" :tabindex="disabled?-1:0" :aria-disabled="disabled" :aria-pressed="selected===index"
          :aria-label="band.frequency+' Hz，'+band.gain+' dB，Q '+band.q+(band.enabled?'':'，已停用')"
          @pointerdown="start($event,index)" @focus="selected=index" @keydown="key($event,index)">
          <title>{{ band.frequency }} Hz · {{ band.gain }} dB · Q {{ band.q }}</title>
        </circle>
        <circle :cx="graphX(band.frequency)" :cy="graphY(band.type>=4?0:band.gain)" r="4.5" class="node-dot"/>
      </g>
    </svg>
  </div>
</template>
