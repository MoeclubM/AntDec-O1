import { computed, onMounted, onUnmounted, ref, shallowRef } from 'vue';
import { AudioControl, outputOperations, type AudioStatus, type ControlStatus, type DeviceInfo,
  type Meters, type OutputSettings, type OutputSetting } from './transports';
import { deviceProfile, Feature } from './devices';
import { readSettings, settingsKey, writeSettings, type SoundSettings } from './settings';

export function useDevice() {
  const audio = new AudioControl();
  const connected = ref(false), busy = ref(false), error = ref(''), clock = ref(Date.now());
  const info = shallowRef<DeviceInfo>(), state = shallowRef<AudioStatus>(), controller = shallowRef<ControlStatus>();
  const output = shallowRef<OutputSettings>();
  const meters = shallowRef<Meters>({ valid: false, left: 0, right: 0 });
  const meterLevels = shallowRef({ left: -90, right: -90 });
  const meterPeaks = shallowRef({ left: -90, right: -90 });
  const peakUntil = { left: 0, right: 0 };
  let meterTick = performance.now();
  function updateMeters(value: Meters) {
    const now = performance.now(), seconds = (now - meterTick) / 1000;
    const levels = { ...meterLevels.value }, peaks = { ...meterPeaks.value };
    for (const ch of ['left', 'right'] as const) {
      const target = value.valid && value[ch] ? Math.max(-90, value[ch] - 121) : -90;
      levels[ch] = value.valid ? Math.max(target, levels[ch] - 24 * seconds) : -90;
      if (!value.valid) { peaks[ch] = -90; peakUntil[ch] = now; }
      else if (target >= peaks[ch]) { peaks[ch] = target; peakUntil[ch] = now + 1000; }
      else if (now > peakUntil[ch]) peaks[ch] = Math.max(levels[ch], peaks[ch] - 12 * (now - Math.max(meterTick, peakUntil[ch])) / 1000);
    }
    meterTick = now; meters.value = value; meterLevels.value = levels; meterPeaks.value = peaks;
  }
  const profile = computed(() => info.value && deviceProfile(info.value));
  const storageKey = computed(() => info.value ? settingsKey(info.value) : '');
  const has = (feature: number) => !!profile.value?.supports(feature);
  const saveState = ref<'idle' | 'local' | 'scheduled' | 'saving' | 'queued' | 'saved' | 'failed'>('idle');
  const saveLabel = computed(() => ({ idle: '', local: '已保存到此浏览器', scheduled: '等待自动保存',
    saving: '保存中', queued: '待设备空闲保存', saved: '已自动保存', failed: '保存失败，稍后自动重试' })[saveState.value]);
  let sound: SoundSettings | undefined, saveTimer: ReturnType<typeof setTimeout> | undefined;
  let outputToRestore: Pick<ControlStatus, 'attenuation' | 'mute'> | undefined;
  let revision = 0, persisted = 0, requested = 0, session = 0, saveAfter = 0, idleSince = 0, saving = false;
  let saveRetries = 0;
  let saveError = '';
  function store(update: Parameters<typeof writeSettings>[1]) {
    const stored = writeSettings(storageKey.value, update);
    if (!stored) error.value = '浏览器无法保存设置，请检查站点存储权限';
    return stored;
  }
  function rememberOutput() {
    const c = controller.value;
    if (!c?.online) return;
    outputToRestore = undefined;
    if (store({ output: { attenuation: c.attenuation, mute: c.mute } }) && revision === persisted)
      saveState.value = 'local';
  }
  function rememberSound(update: Partial<Omit<SoundSettings, 'pending'>>) {
    if (!sound) return;
    sound = { ...sound, ...update, pending: true };
    store({ sound });
    if (has(Feature.manager)) { saveState.value = state.value?.playing ? 'queued' : 'scheduled'; return; }
    ++revision; saveRetries = 0; postponeSave();
  }
  function postponeSave() {
    if (has(Feature.manager)) return;
    if (revision === persisted) return;
    saveAfter = Date.now() + 2000;
    saveState.value = state.value?.playing ? 'queued' : 'scheduled';
    clearTimeout(saveTimer); saveTimer = setTimeout(() => { saveTimer = undefined; void save(); }, 2000);
  }
  function retrySave(message: string) {
    saveState.value = 'failed'; saveError = message; error.value = message;
    const delay = Math.min(30000, 5000 * 2 ** Math.min(saveRetries++, 3));
    clearTimeout(saveTimer); saveTimer = setTimeout(() => { saveTimer = undefined; void save(); }, delay);
  }
  function observeSave(s: AudioStatus) {
    if (has(Feature.manager)) {
      if (s.saveStatus === 1) saveState.value = s.playing ? 'queued' : 'scheduled';
      else if (s.saveStatus === 2) {
        saveState.value = 'saved';
        if (sound?.pending) { sound = { ...sound, pending: false }; store({ sound }); }
      } else if (s.saveStatus === 3) saveState.value = 'failed';
      return;
    }
    if (!s.playing) idleSince ||= Date.now(); else idleSince = 0;
    if (saving) return;
    if (requested && s.saveStatus === 0) {
      requested = 0; saveAfter = Date.now() + 2000; saveState.value = 'scheduled';
    }
    if (requested && (s.saveStatus === 2 || s.saveStatus === 3)) {
      const completed = requested; requested = 0;
      if (s.saveStatus === 2) {
        persisted = completed; saveRetries = 0;
        if (error.value === saveError) error.value = '';
        saveError = '';
        if (revision === completed && sound) { sound = { ...sound, pending: false }; store({ sound }); saveState.value = 'saved'; }
      } else if (revision === completed) retrySave('设备未能保存设置，将稍后自动重试');
    }
    if (revision > persisted && !requested && !saveTimer && saveState.value !== 'failed') void save();
  }
  async function save() {
    if (has(Feature.manager)) return;
    if (!connected.value || saving || requested || revision === persisted) return;
    if (busy.value) {
      clearTimeout(saveTimer); saveTimer = setTimeout(() => { saveTimer = undefined; void save(); }, 250); return;
    }
    if (state.value?.playing) { saveState.value = 'queued'; return; }
    const delay = Math.max(saveAfter, idleSince + 750) - Date.now();
    if (delay > 0) {
      clearTimeout(saveTimer); saveTimer = setTimeout(() => { saveTimer = undefined; void save(); }, delay); return;
    }
    const version = session, savingRevision = revision;
    saving = true; saveState.value = 'saving';
    try {
      await audio.write(0xa7);
      const s = await audio.status();
      if (version !== session || !connected.value) return;
      requested = savingRevision; state.value = s; saving = false;
      saveState.value = revision === savingRevision ? 'queued' : 'scheduled'; observeSave(s);
    } catch (e) {
      if (version === session) retrySave(e instanceof Error ? e.message : String(e));
    } finally { if (version === session) saving = false; }
  }
  let polling = false, timer: ReturnType<typeof setInterval>;
  const issues = computed(() => {
    const c = controller.value, list: string[] = [];
    if (!connected.value) return list;
    if (has(Feature.dac) && c) {
      if (!c.online) list.push('控制通信异常');
      else {
        if (c.power === 'FAULT' || c.power === 'WAIT_POWER') list.push('供电异常');
        if (!c.dac) list.push('解码异常');
        if (c.power === 'READY' && !c.amp) list.push('耳放异常');
      }
    }
    if ((c?.online && c.storageFault) || state.value?.saveStatus === 3) list.push('存储异常');
    if (c?.configSyncFailed || state.value?.managementFault) list.push('配置管理异常');
    return list;
  });
  const status = computed(() => !connected.value ? '未连接' : issues.value.length
    ? issues.value[Math.floor(clock.value / 2000) % issues.value.length]
    : controller.value?.configSyncing ? '同步设置中'
    : has(Feature.dac) && controller.value?.power !== 'READY' ? '启动中' : '正常');
  async function refresh() {
    const version = session;
    const s = await audio.status();
    if (version !== session) return;
    state.value = s; observeSave(s);
    if (has(Feature.dac) || has(Feature.timer)) {
      let c = await audio.controller();
      if (version !== session) return;
      if (has(Feature.output) && c.online && c.advancedOutput) {
        const settings = await audio.outputSettings();
        if (version !== session) return;
        output.value = settings.supported ? settings : undefined;
      } else output.value = undefined;
      if (outputToRestore && c.online && c.power === 'READY' && !c.configSyncing && !c.configSyncFailed && !busy.value) {
        const saved = outputToRestore; outputToRestore = undefined; busy.value = true;
        try {
          if (saved.mute && !c.mute) c = await audio.setController(3, 1);
          if (!c.advancedOutput && c.attenuation !== saved.attenuation)
            c = await audio.setController(2, saved.attenuation);
          if (!saved.mute && c.mute) c = await audio.setController(3, 0);
        } finally { busy.value = false; }
      }
      if (version !== session) return;
      controller.value = c;
    }
    if (has(Feature.meters)) {
      const value = await audio.meters();
      if (version === session) updateMeters(value);
    }
  }
  async function run(fn: () => Promise<void>) {
    if (busy.value) return;
    busy.value = true; error.value = '';
    try { await fn(); }
    catch (e) {
      if (!(e instanceof DOMException && e.name === 'NotFoundError'))
        error.value = e instanceof Error ? e.message : String(e);
    } finally { busy.value = false; }
  }
  async function changeOutput(field: OutputSetting, value: number) {
    await run(async () => {
      controller.value = await audio.setController(outputOperations[field], value);
      await refresh();
    });
  }
  function clear() {
    ++session; clearTimeout(saveTimer); saveTimer = undefined;
    revision = persisted = requested = saveAfter = idleSince = saveRetries = 0;
    saveError = '';
    saving = false; sound = undefined; outputToRestore = undefined; saveState.value = 'idle';
    connected.value = false; info.value = undefined; state.value = undefined; controller.value = undefined; output.value = undefined;
    updateMeters({ valid: false, left: 0, right: 0 });
  }
  async function disconnect() { clear(); await audio.disconnect(); }
  async function connect() {
    await run(async () => {
      if (connected.value) return disconnect();
      try {
        await audio.connect(); info.value = await audio.info();
        const saved = readSettings(storageKey.value);
        if (has(Feature.dac)) outputToRestore = saved.output;
        const s = await audio.status();
        if (has(Feature.eq) || has(Feature.crossfeed)) {
          sound = { dsp: s.dsp, crossfeed: s.crossfeed, preamp: s.preamp,
            bands: has(Feature.eq) ? await audio.bands(info.value.bands) : [], pending: false };
          if (saved.sound?.pending && saved.sound.bands.length === sound.bands.length) {
            sound = saved.sound;
            if (has(Feature.eq)) {
              await audio.write(0xa8, Math.round(sound.preamp * 100));
              for (const [i, band] of sound.bands.entries()) await audio.setBand(i, band);
              await audio.write(0xa2, sound.dsp);
            }
            if (has(Feature.crossfeed)) await audio.write(0xa3, +sound.crossfeed);
          }
        }
        connected.value = true; await refresh();
        if (sound?.pending) rememberSound({});
      } catch (e) { await disconnect(); throw e; }
    });
  }
  const onDisconnect = (event: USBConnectionEvent) => {
    if (event.device === audio.device) { clear(); void audio.disconnect(); }
  };
  onMounted(() => {
    navigator.usb?.addEventListener('disconnect', onDisconnect);
    timer = setInterval(() => {
      clock.value = Date.now();
      if (!connected.value || busy.value || polling) return;
      polling = true;
      void refresh().catch(e => { error.value = e instanceof Error ? e.message : String(e); })
        .finally(() => { polling = false; });
    }, 250);
  });
  onUnmounted(() => { clearInterval(timer); navigator.usb?.removeEventListener('disconnect', onDisconnect); void disconnect(); });
  return { audio, connected, busy, error, info, state, controller, output, meters, meterLevels, meterPeaks, profile, storageKey,
    has, issues, status, saveState, saveLabel, rememberOutput, rememberSound, postponeSave,
    connect, disconnect, refresh, run, changeOutput };
}
