<script setup lang="ts">
import { computed, reactive, ref, watch } from 'vue';
import type { OutputSettings, OutputSetting } from './transports';

const props = defineProps<{ settings?: OutputSettings; disabled: boolean; unavailable: string;
  rate: number; format: number }>();
const emit = defineEmits<{ change: [field: OutputSetting, value: number] }>();
const form = reactive({ balance: 0, startup: 40, ceiling: 0, fadeUp: 2, fadeDown: 32,
  automuteLevel: 8, automuteTime: 15, invert: 0, automuteEnabled: true });
const dragging = ref(false);
watch(() => props.settings, settings => {
  if (!settings?.supported) return;
  const balance = form.balance;
  Object.assign(form, settings);
  if (dragging.value) form.balance = balance;
}, { immediate: true });
const balanceDb = computed({ get: () => form.balance / 2,
  set: (value: number) => { if (Number.isFinite(value)) form.balance = Math.round(value * 2); } });
const startupDb = computed({ get: () => -form.startup / 2,
  set: (value: number) => { if (Number.isFinite(value)) form.startup = Math.round(-value * 2); } });
const ceilingDb = computed({ get: () => -form.ceiling / 2,
  set: (value: number) => { if (Number.isFinite(value)) form.ceiling = Math.round(-value * 2); } });
const balanceText = computed(() => !form.balance ? '居中' : (form.balance < 0 ? '左 ' : '右 ') + Math.abs(form.balance / 2).toFixed(1) + ' dB');
const delayText = computed(() => {
  if (props.format && props.format !== 1) return '';
  const rate = props.format === 1 && props.rate ? props.rate : 48000;
  return `约 ${Math.round(262144 / form.automuteTime / rate * 1000)} ms · ${rate / 1000} kHz`;
});
function change(field: OutputSetting, value: number | boolean | undefined) {
  if (value !== undefined) emit('change', field, Number(value));
}
function changeBalance() { dragging.value = false; change('balance', form.balance); }
</script>

<template>
  <div class="output-settings">
    <el-alert v-if="unavailable" :title="unavailable" type="info" :closable="false"/>
    <section class="setting-section"><h2>声道</h2>
      <el-form label-position="left" label-width="108px" :disabled="disabled || !settings?.supported">
        <el-form-item label="左右平衡"><div class="balance-control">
          <span class="subtle">左</span><el-slider v-model="balanceDb" :min="-12" :max="12" :step=".5"
            :format-tooltip="()=>balanceText" @input="dragging=true" @change="changeBalance"/>
          <span class="subtle">右</span><span class="balance-value">{{ balanceText }}</span>
        </div></el-form-item>
        <el-form-item label="极性反转"><el-checkbox :model-value="!!(form.invert&1)" @change="(value:boolean|string|number)=>change('invert',(form.invert&2)|(value?1:0))">左声道</el-checkbox>
          <el-checkbox :model-value="!!(form.invert&2)" @change="(value:boolean|string|number)=>change('invert',(form.invert&1)|(value?2:0))">右声道</el-checkbox></el-form-item>
      </el-form>
    </section>
    <section class="setting-section"><h2>音量策略</h2>
      <el-form label-position="left" label-width="108px" :disabled="disabled || !settings?.supported">
        <el-form-item label="开机音量"><el-input-number v-model="startupDb" :min="-100" :max="ceilingDb" :step=".5" :precision="1"
          controls-position="right" @change="()=>change('startup',form.startup)"/><span class="unit-label subtle">dB</span></el-form-item>
        <el-form-item label="最大音量"><el-input-number v-model="ceilingDb" :min="-100" :max="0" :step=".5" :precision="1"
          controls-position="right" @change="()=>change('ceiling',form.ceiling)"/><span class="unit-label subtle">dB</span></el-form-item>
        <el-form-item label="淡入速度"><el-select v-model="form.fadeUp" class="preset-select" @change="(value:number)=>change('fadeUp',value)">
          <el-option label="慢" :value="1"/><el-option label="标准" :value="2"/><el-option label="快" :value="8"/></el-select></el-form-item>
        <el-form-item label="淡出速度"><el-select v-model="form.fadeDown" class="preset-select" @change="(value:number)=>change('fadeDown',value)">
          <el-option label="慢" :value="8"/><el-option label="标准" :value="32"/><el-option label="快" :value="128"/></el-select></el-form-item>
      </el-form>
    </section>
    <section class="setting-section"><h2>自动静音</h2>
      <el-form label-position="left" label-width="108px" :disabled="disabled || !settings?.supported">
        <el-form-item label="启用"><el-switch :model-value="form.automuteEnabled" @change="(value:boolean|string|number)=>change('automuteEnabled',!!value)"/></el-form-item>
        <el-form-item label="静音门限"><el-select v-model="form.automuteLevel" class="preset-select" :disabled="!form.automuteEnabled" @change="(value:number)=>change('automuteLevel',value)">
          <el-option label="低" :value="1"/><el-option label="标准" :value="8"/><el-option label="高" :value="32"/></el-select></el-form-item>
        <el-form-item label="静音延时"><el-select v-model="form.automuteTime" class="preset-select" :disabled="!form.automuteEnabled" @change="(value:number)=>change('automuteTime',value)">
          <el-option label="短" :value="55"/><el-option label="标准" :value="15"/><el-option label="长" :value="5"/></el-select>
          <span v-if="delayText" class="unit-label subtle">{{ delayText }}</span></el-form-item>
      </el-form>
    </section>
  </div>
</template>
