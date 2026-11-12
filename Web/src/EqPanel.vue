<script setup lang="ts">
import { computed, onUnmounted, ref, watch } from 'vue';
import { Refresh, Download, Upload, Plus, Delete } from '@element-plus/icons-vue';
import { AudioControl, type AudioStatus, type Band } from './transports';
import EqChart from './EqChart.vue';
import { decodeEqFile, exportEqFile, parseEqFile, type EqFile } from './eqFiles';
import { readSettings, writeSettings, type SoundSettings } from './settings';

const props = defineProps<{ audio: AudioControl; connected: boolean; storageKey: string; state?: AudioStatus;
  maxRate: number; bandCount: number; disabled: boolean }>();
const emit = defineEmits<{ applied: [settings: Pick<SoundSettings, 'bands' | 'preamp' | 'dsp'>]; editing: []; refresh: []; error: [message: string] }>();
const bands = ref<Band[]>([]), live = ref(true), automatic = ref(true), preamp = ref(0), selected = ref(-1);
const label = ref(''), failed = ref(false), loading = ref(false);
const rows = computed(() => bands.value.map((band, index) => ({ band, index })).filter(row => row.band.type !== 0));
const locked = computed(() => props.disabled || loading.value);
const dirty = new Set<number>();
let normalize = false, preampDirty = false, loadVersion = 0;
let timer: ReturnType<typeof setTimeout> | undefined, sending: Promise<void> | undefined;
const types = ['关闭', '峰值', '低架', '高架', '低通', '高通'];
const rate = computed(() => props.state?.rate || 48000);
const bypass = computed(() => !!props.state?.playing && (props.state.format !== 1 || rate.value > props.maxRate));
const active = computed(() => !!props.state?.playing && !bypass.value && props.state?.dsp === 5);
const effectivePreamp = computed(() => automatic.value
  ? -Math.max(0,...bands.value.filter(b=>b.enabled && b.type!==0).map(b=>Number(b.gain)||0)) : Number(preamp.value)||0);
const emptyBand = (): Band => ({frequency:1000,gain:0,q:1,type:0,enabled:false});
const message = (e: unknown) => e instanceof Error ? e.message : String(e);
const capacity = computed(() => rows.value.length >= props.bandCount);
const factoryFrequencies = [32,64,125,250,500,1000,2000,4000,8000,16000];

function cacheDraft(pending = !!dirty.size || preampDirty || !!sending) {
  if (!props.connected || !props.storageKey) return;
  if (!writeSettings(props.storageKey, { eqDraft: { bands: bands.value.map(b=>({...b})), automatic: automatic.value,
    preamp: preamp.value, live: live.value, pending } }))
    emit('error', '浏览器无法保存 EQ 草稿，请检查站点存储权限');
}
async function load(restoreDraft = false) {
  clearTimeout(timer); timer = undefined;
  const version = ++loadVersion;
  loading.value = true;
  try {
    if (sending) await sending.catch(()=>{});
    const [s, b] = await Promise.all([props.audio.status(), props.audio.bands(props.bandCount)]);
    if (version !== loadVersion || !props.connected) return;
    normalize = b.length===factoryFrequencies.length && b.every((band,i)=>band.enabled && band.gain===0
      && band.frequency===factoryFrequencies[i] && band.type===(i===0?2:i===9?3:1) && band.q===(i===0 || i===9?.71:1));
    bands.value = normalize ? b.map(emptyBand) : b;
    automatic.value = s.preamp === 0; preamp.value = s.preamp;
    selected.value=-1; dirty.clear(); preampDirty=false; failed.value=false; label.value='';
    const draft = readSettings(props.storageKey).eqDraft;
    live.value = draft?.live ?? true;
    if (restoreDraft && draft?.pending && draft.bands.length === props.bandCount) {
      bands.value = draft.bands; automatic.value = draft.automatic; preamp.value = draft.preamp; normalize = false;
      bands.value.forEach((_,i)=>dirty.add(i)); preampDirty=true;
      label.value=valid()?(live.value?'待应用':'草稿已保存'):'参数超出范围'; schedule();
    }
    cacheDraft();
  } finally { if (version===loadVersion) loading.value=false; }
}
function valid() {
  return (automatic.value || Number.isFinite(preamp.value) && preamp.value >= -36 && preamp.value < 0)
    && bands.value.every(b => Number.isFinite(b.frequency) && b.frequency >= 20 && b.frequency <= 40000
      && Number.isFinite(b.gain) && Math.abs(b.gain) <= 18 && Number.isFinite(b.q) && b.q >= .1 && b.q <= 16);
}
function schedule() {
  if (live.value && valid() && props.connected && !timer)
    timer = setTimeout(() => { timer=undefined; void apply(); }, 160);
}
function edited(index?: number) {
  if (normalize) { bands.value.forEach((_,i)=>dirty.add(i)); normalize=false; }
  if (index === undefined) preampDirty=true; else dirty.add(index);
  failed.value=false; label.value=valid()?(live.value?'待应用':'草稿已保存'):'参数超出范围'; cacheDraft(); emit('editing'); schedule();
}
function change(index: number, values: Partial<Band>) {
  if (locked.value) return;
  Object.assign(bands.value[index],values); selected.value=index; edited(index);
}
function addBand(frequency?: number, gain=0) {
  if (locked.value) return;
  const index=bands.value.findIndex(b=>b.type===0);
  if (index<0) { emit('error',`当前设备最多支持 ${props.bandCount} 个频段`); return; }
  if (frequency===undefined) {
    const frequencies=[20,...rows.value.map(r=>r.band.frequency).filter(f=>f<20000),20000].sort((a,b)=>a-b);
    const gaps=frequencies.slice(1).map((f,i)=>({size:f/frequencies[i],frequency:Math.sqrt(f*frequencies[i])}));
    frequency=rows.value.length?Math.round(gaps.sort((a,b)=>b.size-a.size)[0].frequency):1000;
  }
  bands.value[index]={frequency,gain,q:1,type:1,enabled:true}; selected.value=index; edited(index);
}
function removeBand(index: number) {
  if (locked.value) return;
  bands.value[index]=emptyBand(); if (selected.value===index) selected.value=-1; edited(index);
}
function changeType(index: number) {
  if (bands.value[index].type>=4) bands.value[index].gain=0;
  edited(index);
}
function clearBands() {
  bands.value=Array.from({length:props.bandCount},emptyBand);
  selected.value=-1; automatic.value=true; preamp.value=0;
  bands.value.forEach((_,i)=>dirty.add(i)); edited();
}
function setAutomatic() {
  if (!automatic.value && preamp.value===0) preamp.value=-.1;
  edited();
}
async function flush(): Promise<void> {
  clearTimeout(timer); timer=undefined;
  if (sending) await sending;
  if (!dirty.size && !preampDirty) return;
  if (!valid()) throw new Error('请检查频率、增益、Q 和前级增益');
  const version=loadVersion;
  const indices=[...dirty], snapshot=bands.value.map(b=>({...b})), gain=automatic.value?0:Math.round(preamp.value*100);
  const updatePreamp=preampDirty;
  dirty.clear(); preampDirty=false; label.value='应用中';
  sending=(async()=>{
    try {
      if (updatePreamp) await props.audio.write(0xa8,gain);
      for (const index of indices) {
        if (version!==loadVersion || !props.connected) throw new Error('设备连接已改变，请重新读取 EQ');
        await props.audio.setBand(index,snapshot[index]);
      }
      if (version!==loadVersion || !props.connected) return;
      await props.audio.write(0xa2,5);
      if (version!==loadVersion || !props.connected) return;
      failed.value=false; label.value=dirty.size || preampDirty?'待应用':'';
      emit('applied', { bands: snapshot, preamp: gain / 100, dsp: 5 });
      cacheDraft(!!dirty.size || preampDirty); emit('refresh');
    } catch(e) {
      if (version===loadVersion) {
        indices.forEach(i=>dirty.add(i)); preampDirty ||= updatePreamp;
        failed.value=true; label.value='应用失败'; cacheDraft();
      }
      throw e;
    }
  })();
  try { await sending; } finally { sending=undefined; }
  if (dirty.size || preampDirty) schedule();
}
async function apply() {
  try { await flush(); } catch(e) { emit('error',message(e)); }
}

const fileInput=ref<HTMLInputElement>(), importOpen=ref(false), importName=ref(''), importError=ref('');
const imported=ref<EqFile>();
const tooMany=computed(()=>!!imported.value && imported.value.bands.length>props.bandCount);
async function chooseFile(event: Event) {
  const input=event.target as HTMLInputElement, file=input.files?.[0];
  if (!file) return;
  importName.value=file.name; imported.value=undefined; importError.value=''; importOpen.value=true;
  try { imported.value=parseEqFile(decodeEqFile(await file.arrayBuffer())); }
  catch(e) { importError.value=message(e); }
  finally { input.value=''; }
}
function commitImport() {
  if (!imported.value || tooMany.value || locked.value) return;
  const value=imported.value;
  bands.value=Array.from({length:props.bandCount},(_,i)=>value.bands[i]?{...value.bands[i]}:emptyBand());
  automatic.value=value.automatic; preamp.value=value.preamp; selected.value=-1;
  bands.value.forEach((_,i)=>dirty.add(i)); edited(); importOpen.value=false;
}
function download() {
  const url=URL.createObjectURL(new Blob([exportEqFile(bands.value,preamp.value,automatic.value)],{type:'application/json'}));
  const link=document.createElement('a'); link.href=url; link.download='AntDec-EQ.json'; link.click();
  setTimeout(()=>URL.revokeObjectURL(url),1000);
}
watch(live, () => { cacheDraft(); if (live.value) schedule(); else { clearTimeout(timer); timer=undefined; } });
watch(()=>props.connected, connected=>{
  if (connected) void load(true).catch(e=>emit('error',message(e)));
  else { ++loadVersion; loading.value=false; clearTimeout(timer); timer=undefined; bands.value=[];
    dirty.clear(); preampDirty=false; selected.value=-1; label.value=''; importOpen.value=false; }
},{immediate:true});
onUnmounted(()=>{++loadVersion;clearTimeout(timer);});
defineExpose({flush,load});
</script>

<template>
  <div class="eq-panel" v-loading="loading">
    <div class="panel-toolbar">
      <div class="inline"><el-switch v-model="live" active-text="实时应用" :disabled="locked"/>
        <span v-if="label" class="subtle" :class="{'danger-text':failed}">{{ label }}</span></div>
      <div class="inline eq-actions">
        <el-button v-if="!live || failed" :disabled="locked" @click="apply">应用 EQ</el-button>
        <el-button :icon="Plus" :disabled="locked || capacity" @click="addBand()">添加频段</el-button>
        <span class="subtle eq-capacity">{{ rows.length }} / {{ bandCount }}</span>
        <el-button :icon="Upload" :disabled="locked" @click="fileInput?.click()">导入</el-button>
        <el-button :icon="Download" :disabled="locked || !valid()" @click="download">导出</el-button>
        <el-button :icon="Refresh" :disabled="locked" @click="load().catch(e=>emit('error',message(e)))">读取</el-button>
        <el-button :icon="Delete" :disabled="locked || !rows.length" @click="clearBands">清空</el-button>
      </div>
      <input ref="fileInput" class="file-input" type="file" accept=".txt,.json,.peq,text/plain,application/json" @change="chooseFile">
    </div>
    <EqChart v-model:selected="selected" :bands="bands" :preamp="effectivePreamp" :rate="rate" :disabled="locked" :valid="valid()"
      :caption="active?'生效中':bypass?'当前格式直通':'未启用'" @add="addBand" @change="change" @remove="removeBand"/>
    <div class="eq-preamp"><span>前级增益</span>
      <el-input-number v-model="preamp" :min="-36" :max="automatic?0:-.01" :step=".1" :precision="2" controls-position="right" :disabled="locked || automatic" @change="edited()"/>
      <span class="subtle">dB</span><el-checkbox v-model="automatic" :disabled="locked" @change="setAutomatic">自动余量</el-checkbox>
      <span v-if="automatic" class="subtle">{{ effectivePreamp.toFixed(1) }} dB</span>
    </div>
    <el-table :data="rows" row-key="index" size="small" class="eq-table" empty-text="暂无频段"
      :row-class-name="({row}: {row: {index:number}})=>row.index===selected?'selected-band':''" @row-click="(row:{index:number})=>selected=row.index">
      <el-table-column label="启用" width="60"><template #default="{row}"><el-checkbox v-model="row.band.enabled" :aria-label="'启用 '+row.band.frequency+' Hz'" :disabled="locked" @change="edited(row.index)"/></template></el-table-column>
      <el-table-column label="类型" width="118"><template #default="{row}"><el-select v-model="row.band.type" :disabled="locked" @change="changeType(row.index)"><el-option v-for="(name,i) in types.slice(1)" :key="i" :label="name" :value="i+1"/></el-select></template></el-table-column>
      <el-table-column label="频率 · Hz" width="152"><template #default="{row}"><el-input-number v-model="row.band.frequency" :min="20" :max="40000" :precision="0" :step="1" controls-position="right" :disabled="locked" @change="edited(row.index)"/></template></el-table-column>
      <el-table-column label="增益 · dB" min-width="260"><template #default="{row}"><div class="gain-control"><el-slider v-model="row.band.gain" :min="-18" :max="18" :step=".1" :disabled="locked || row.band.type>=4" @input="edited(row.index)"/><el-input-number v-model="row.band.gain" :min="-18" :max="18" :step=".1" :precision="2" controls-position="right" :disabled="locked || row.band.type>=4" @change="edited(row.index)"/></div></template></el-table-column>
      <el-table-column label="Q" width="128"><template #default="{row}"><el-input-number v-model="row.band.q" :min=".1" :max="16" :step=".05" :precision="2" controls-position="right" :disabled="locked" @change="edited(row.index)"/></template></el-table-column>
      <el-table-column width="52" align="center"><template #default="{row}"><el-button :icon="Delete" text :disabled="locked" :aria-label="'删除 '+row.band.frequency+' Hz 频段'" @click.stop="removeBand(row.index)"/></template></el-table-column>
    </el-table>
    <div v-if="bypass" class="table-note">{{ props.state?.format===1?'当前采样率超过 EQ 处理上限（'+maxRate/1000+' kHz）':'DSD 保持直通' }}</div>

    <el-dialog v-model="importOpen" title="导入 EQ" width="720px" class="eq-import-dialog" :close-on-click-modal="false">
      <div class="import-file-name">{{ importName }}</div>
      <el-alert v-if="importError" :title="importError" type="error" :closable="false" show-icon/>
      <template v-if="imported">
        <div class="import-summary"><span>{{ imported.format }}</span><span>{{ imported.bands.length }} 个频段</span>
          <span>前级 {{ imported.automatic?'自动余量':imported.preamp+' dB' }}</span></div>
        <el-alert v-if="tooMany" :title="'文件有 '+imported.bands.length+' 个频段，当前设备最多支持 '+bandCount+' 个。请先在源文件中调整。'" type="error" :closable="false" show-icon/>
        <el-alert v-for="warning in imported.warnings" :key="warning" :title="warning" type="warning" :closable="false" show-icon/>
        <el-table :data="imported.bands" size="small" max-height="320" empty-text="无频段">
          <el-table-column label="启用" width="60"><template #default="{row}">{{ row.enabled?'是':'否' }}</template></el-table-column>
          <el-table-column label="类型" width="90"><template #default="{row}">{{ types[row.type] }}</template></el-table-column>
          <el-table-column prop="frequency" label="频率 · Hz"/><el-table-column prop="gain" label="增益 · dB"/>
          <el-table-column prop="q" label="Q"/>
        </el-table>
      </template>
      <template #footer><el-button @click="importOpen=false">取消</el-button>
        <el-button type="primary" :disabled="!imported || tooMany || locked" @click="commitImport">{{ live?'导入并应用':'导入草稿' }}</el-button></template>
    </el-dialog>
  </div>
</template>
