<script setup lang="ts">
import { computed, ref, watch } from 'vue';
import { ElMessageBox } from 'element-plus';
import { Odometer, Headset, Operation, Setting, Connection, SwitchButton, Mute, Microphone, Sunny, Moon } from '@element-plus/icons-vue';
import EqPanel from './EqPanel.vue';
import OutputPanel from './OutputPanel.vue';
import { useDevice } from './useDevice';
import { duration, Feature, filters, presets, modes } from './devices';
import { dark } from './theme';

const { audio, connected, busy, error, info, state, controller, output, meters, meterLevels, meterPeaks, profile, storageKey,
  has, issues, status, saveState, saveLabel, rememberOutput, rememberSound, postponeSave,
  connect, disconnect, refresh, run, changeOutput } = useDevice();
const page = ref('overview'), eq = ref<InstanceType<typeof EqPanel>>();
const pages = [{key:'overview',name:'概览',icon:Odometer},{key:'sound',name:'声音设置',icon:Headset},
  {key:'eq',name:'均衡器',icon:Operation},{key:'device',name:'设备设置',icon:Setting}];
const title = computed(()=>pages.find(p=>p.key===page.value)?.name);
const editingFilter = ref(false), filter = ref(0), dspDirty = ref(false), preset = ref(0), crossfeed = ref(false);
const mode = ref(0), modeDirty = ref(false), volume = ref(-20), volumeDragging = ref(false);
const maximumVolume = computed(()=>-(output.value?.ceiling??0)/2);
const positionToVolume = (position:number)=>Math.round((maximumVolume.value-(100+maximumVolume.value)*(1-position/100)**2)*2)/2;
const volumePosition = computed({
  get:()=>100*(1-Math.sqrt(Math.max(0,maximumVolume.value-(Number.isFinite(volume.value)?volume.value:-(controller.value?.attenuation??40)/2))/(100+maximumVolume.value||1))),
  set:(position:number)=>{volume.value=positionToVolume(position);},
});
const usable = computed(()=>connected.value && !busy.value);
const controllerReady = computed(()=>usable.value && !!controller.value?.online && !controller.value.configSyncing && !controller.value.configSyncFailed);
const outputSaveLabel = computed(()=>{
  const c=controller.value;
  if (!c?.online) return '';
  if (!c.filterPersistent) return '控制板固件尚不支持断电记忆';
  if (c.settingsPending) return c.storageFault?'保存失败，稍后自动重试':'等待自动保存';
  return '已保存到设备';
});
const outputUnavailable = computed(()=>!connected.value ? '' : !has(Feature.output) ? '这些设置需要更新音频板固件'
  : !controller.value?.online ? '控制板未连接' : !controller.value.advancedOutput ? '这些设置需要更新控制板固件'
  : controller.value.configSyncFailed ? '设备配置同步失败，将稍后自动重试'
  : controller.value.configSyncing ? '设备配置同步中'
  : !output.value?.supported ? '输出设置尚未就绪' : '');
const stream = computed(()=>{
  const s=state.value;
  if (!connected.value) return '—';
  if (!s?.playing) return '空闲';
  if (s.format===2 || s.format===3) return (s.format===2?'DoP ':'DSD ')+Math.round(s.rate/44100);
  return 'PCM '+(s.rate/1000).toLocaleString('en-US',{maximumFractionDigits:1})+' kHz';
});
const bits = computed(()=>state.value?.playing ? (state.value.format===2 || state.value.format===3 ? '1 bit' : state.value.bits+' bit') : '—');
const elapsed = computed(()=>controller.value?.online ? duration(controller.value.playSeconds) : '—');
const dspLabel = computed(()=>!state.value?.playing ? '—' : state.value.format!==1 || state.value.rate>(profile.value?.maxDspRate||0)
  ? '直通' : presets[state.value.dsp]);
const db = (code:number) => code ? code-121 : -120;
const meterHeight = (level:number) => (level+90)/90*100;
const meterText = (code:number) => !meters.value.valid ? '—' : code ? db(code)+' dBFS' : '−∞';
watch(controller,c=>{
  if (!c?.online) return;
  if (!editingFilter.value) filter.value=c.filter;
  if (!volumeDragging.value) volume.value=-c.attenuation/2;
});
watch(state,s=>{
  if (!s) return;
  if (!dspDirty.value) { preset.value=s.dsp; crossfeed.value=s.crossfeed; }
  if (!modeDirty.value) mode.value=s.usbMode;
});
watch(connected,()=>{editingFilter.value=dspDirty.value=modeDirty.value=false;});
async function setVolume() {
  if (!Number.isFinite(volume.value)) return;
  volumeDragging.value=false;
  await run(async()=>{controller.value=await audio.setController(2,Math.round(-volume.value*2));rememberOutput();});
}
function finishVolumeInput() {
  volumeDragging.value=false;
  if (!Number.isFinite(volume.value)) volume.value=-(controller.value?.attenuation??40)/2;
}
async function setFilter() {
  const selected=filter.value; editingFilter.value=true;
  await run(async()=>{controller.value=await audio.setController(1,selected);});
  editingFilter.value=false;
  if (controller.value?.online) filter.value=controller.value.filter;
}
async function toggleMute() {
  await run(async()=>{controller.value=await audio.setController(3,controller.value?.mute?0:1);rememberOutput();});
}
async function setDsp() {
  await run(async()=>{
    await eq.value?.flush(); await audio.write(0xa2,preset.value); await audio.write(0xa3,+crossfeed.value);
    rememberSound({dsp:preset.value,crossfeed:crossfeed.value});
    dspDirty.value=false; await refresh();
  });
}
async function resetTime() {
  const confirmed=await ElMessageBox.confirm('累计播放时间将清零，是否继续？','清零播放时间',
    {confirmButtonText:'清零',cancelButtonText:'取消',type:'warning'}).then(()=>true).catch(()=>false);
  if (!confirmed) return;
  await run(async()=>{controller.value=await audio.setController(5,0);});
}
async function switchMode() {
  if (mode.value===state.value?.usbMode) return;
  const confirmed=await ElMessageBox.confirm('切换为“'+modes[mode.value]+'”将中断播放并重新连接设备。','切换 USB 模式',
    {confirmButtonText:'切换',cancelButtonText:'取消',type:'warning'}).then(()=>true).catch(()=>false);
  if (!confirmed) return;
  await run(async()=>{await audio.write(0xa1,mode.value);await disconnect();modeDirty.value=false;});
}
function refreshAfterEq() { void refresh().catch(e=>{error.value=String(e);}); }
const usbSupported='usb' in navigator;
</script>

<template>
  <el-container class="console-shell">
    <el-aside width="204px" class="sidebar">
      <div class="brand" aria-label="AntDec">Ant<span>Dec</span></div>
      <el-menu :default-active="page" @select="page=$event">
        <el-menu-item v-for="item in pages" :key="item.key" :index="item.key" :disabled="item.key==='eq' && connected && !has(Feature.eq)">
          <el-icon><component :is="item.icon"/></el-icon><span>{{ item.name }}</span>
        </el-menu-item>
      </el-menu>
    </el-aside>
    <el-container class="workspace">
      <el-header class="topbar"><div class="device-heading">{{ info?.model || '设备控制台' }}</div>
        <div class="inline"><span class="system-status" :class="{fault:issues.length,offline:!connected}"><i/>{{ status }}</span>
          <el-button :icon="dark?Sunny:Moon" circle :aria-label="dark?'切换浅色模式':'切换深色模式'" :title="dark?'切换浅色模式':'切换深色模式'" @click="dark=!dark"/>
          <el-button :icon="connected?SwitchButton:Connection" :loading="busy" :disabled="!usbSupported" :type="connected?'default':'primary'" @click="connect">{{ connected?'断开':'连接设备' }}</el-button></div>
      </el-header>
      <el-main>
        <div class="content-heading"><h1>{{ title }}</h1><div v-if="connected && (page==='sound' || page==='eq')" class="inline">
          <span v-if="saveLabel" class="subtle" role="status" :class="{'danger-text':saveState==='failed'}">{{ saveState==='local'?saveLabel:(has(Feature.manager)?'设置 · ':'音效 · ')+saveLabel }}</span></div></div>
        <el-alert v-if="error" :title="error" type="error" show-icon @close="error=''"/>
        <el-alert v-if="!usbSupported" title="当前浏览器不支持 USB 设备访问" type="warning" :closable="false"/>

        <div v-show="page==='overview'">
          <div class="metrics-strip"><div><span>音频流</span><strong>{{ stream }}</strong><small>{{ bits }}</small></div>
            <div><span>输出音量</span><strong>{{ controller?.online?(-controller.attenuation/2).toFixed(1)+' dB':'—' }}</strong><small>{{ controller?.mute?'静音':' ' }}</small></div>
            <div v-if="!connected || has(Feature.timer)"><span>累计播放</span><strong class="time-value">{{ elapsed }}</strong><small> </small></div></div>
          <div class="overview-body">
            <section class="overview-state"><el-descriptions :column="1" :border="false">
              <el-descriptions-item label="数字滤波">{{ controller?.online?'F'+controller.filter+' · '+filters[controller.filter]:'—' }}</el-descriptions-item>
              <el-descriptions-item label="音效">{{ dspLabel }}</el-descriptions-item>
              <el-descriptions-item label="USB 模式">{{ connected?modes[state?.usbMode??0]:'—' }}</el-descriptions-item>
              <el-descriptions-item label="交叉馈送">{{ connected?(state?.crossfeed?'开启':'关闭'):'—' }}</el-descriptions-item>
            </el-descriptions></section>
            <section v-if="!connected || has(Feature.meters)" class="level-panel"><h2>数字电平 <span class="subtle">dBFS</span></h2>
              <div class="meter-pair"><div class="meter-scale"><span v-for="n in [0,-18,-36,-54,-72,-90]" :key="n">{{ n }}</span></div>
                <div v-for="ch in ['left','right'] as const" :key="ch" class="meter-channel"><div class="meter-track" role="meter" :aria-label="ch==='left'?'左声道电平':'右声道电平'" :aria-valuenow="Math.max(-90,db(meters[ch]))" aria-valuemin="-90" aria-valuemax="0">
                  <div class="meter-fill" :style="{height:meterHeight(meterLevels[ch])+'%'}"/>
                  <div v-if="meters.valid && meterPeaks[ch]>-90" class="meter-peak" :style="{bottom:meterHeight(meterPeaks[ch])+'%'}"/>
                </div><span>{{ ch==='left'?'L':'R' }}</span><output>{{ meterText(meters[ch]) }}</output></div>
              </div>
            </section>
          </div>
        </div>

        <div v-show="page==='sound'" class="settings-page">
          <section v-if="!connected || has(Feature.dac)" class="setting-section"><h2 class="output-heading">输出<span v-if="!output?.managed" class="subtle" role="status">{{ outputSaveLabel }}</span></h2>
            <el-form label-position="left" label-width="108px" class="sound-form" :disabled="!controllerReady">
              <el-form-item label="音量"><div class="volume-control"><el-slider v-model="volumePosition" :min="0" :max="100" :step=".1" :disabled="maximumVolume===-100" :format-tooltip="(p:number)=>positionToVolume(p).toFixed(1)+' dB'" @input="volumeDragging=true" @change="setVolume"/>
                <el-input-number v-model="volume" :min="-100" :max="maximumVolume" :step=".5" :precision="1" controls-position="right" aria-label="输出音量 dB" @focus="volumeDragging=true" @blur="finishVolumeInput" @change="setVolume"/><span class="subtle">dB</span>
                <el-button :icon="controller?.mute?Microphone:Mute" :type="controller?.mute?'warning':'default'" @click="toggleMute">{{ controller?.mute?'取消静音':'静音' }}</el-button></div></el-form-item>
              <el-form-item label="数字滤波"><el-select v-model="filter" class="filter-select" @change="setFilter"><el-option v-for="(name,i) in filters" :key="i" :value="i" :label="'F'+i+' · '+name"/></el-select></el-form-item>
            </el-form>
          </section>
          <OutputPanel v-if="!connected || has(Feature.dac)" :settings="output" :disabled="!controllerReady || !output?.ready" :unavailable="outputUnavailable"
            :rate="state?.rate??0" :format="state?.format??0"
            @change="changeOutput"/>
          <section v-if="!connected || has(Feature.eq) || has(Feature.crossfeed)" class="setting-section"><h2>音效</h2><el-form label-position="left" label-width="108px" :disabled="!usable">
            <el-form-item label="预设"><el-select v-model="preset" class="preset-select" @change="dspDirty=true"><el-option v-for="(name,i) in presets" :key="i" :value="i" :label="name"/></el-select></el-form-item>
            <el-form-item v-if="!connected || has(Feature.crossfeed)" label="交叉馈送"><el-switch v-model="crossfeed" @change="dspDirty=true"/></el-form-item>
            <el-form-item><el-button type="primary" :disabled="!dspDirty" @click="setDsp">应用音效</el-button></el-form-item>
          </el-form></section>
        </div>

        <div v-show="page==='eq'"><EqPanel ref="eq" :audio="audio" :connected="connected && has(Feature.eq)" :storage-key="storageKey" :state="state" :max-rate="profile?.maxDspRate || 192000" :band-count="profile?.bandCount || 10" :disabled="!usable"
          @applied="rememberSound" @editing="postponeSave" @refresh="refreshAfterEq" @error="error=$event"/></div>

        <div v-show="page==='device'" class="settings-page">
          <section class="setting-section"><h2>设备</h2><el-descriptions :column="1" label-width="108px">
            <el-descriptions-item label="型号">{{ profile?.name || '—' }}</el-descriptions-item>
            <el-descriptions-item label="PCM 上限">{{ info?info.maxPcmRate/1000+' kHz':'—' }}</el-descriptions-item>
            <el-descriptions-item v-if="!connected || has(Feature.eq)" label="EQ 处理上限">{{ info?info.maxDspRate/1000+' kHz':'—' }}</el-descriptions-item>
          </el-descriptions></section>
          <section v-if="!connected || has(Feature.timer)" class="setting-section"><h2>播放统计</h2><div class="setting-line"><span>累计播放</span><strong class="time-value">{{ elapsed }}</strong><el-button :disabled="!controllerReady" @click="resetTime">清零</el-button></div></section>
          <section v-if="!connected || has(Feature.usbMode)" class="setting-section"><h2>USB</h2><el-form label-position="left" label-width="108px" :disabled="!usable"><el-form-item label="模式"><el-select v-model="mode" class="filter-select" @change="modeDirty=true"><el-option v-for="(name,i) in modes" :key="i" :label="name" :value="i"/></el-select><el-button :disabled="!modeDirty" @click="switchMode">切换</el-button></el-form-item></el-form></section>
          <el-collapse v-if="connected && has(Feature.dac)"><el-collapse-item title="诊断信息" name="diagnostics"><el-descriptions :column="2" border>
            <el-descriptions-item label="PG1">{{ controller?.online?((controller?.pg1??0)/1000).toFixed(2)+' V':'—' }}</el-descriptions-item>
            <el-descriptions-item label="PG2">{{ controller?.online?((controller?.pg2??0)/1000).toFixed(2)+' V':'—' }}</el-descriptions-item>
            <el-descriptions-item label="控制板供电估算">{{ controller?.online?((controller?.vdda??0)/1000).toFixed(2)+' V':'—' }}</el-descriptions-item>
            <el-descriptions-item label="板间通信">{{ controller?.online?'在线':'离线' }}</el-descriptions-item>
          </el-descriptions></el-collapse-item></el-collapse>
        </div>
      </el-main>
    </el-container>
  </el-container>
</template>
