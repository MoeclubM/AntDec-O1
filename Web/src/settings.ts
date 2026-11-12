import type { Band, ControlStatus, DeviceInfo } from './transports';

export interface SoundSettings {
  dsp: number;
  crossfeed: boolean;
  preamp: number;
  bands: Band[];
  pending: boolean;
}
export interface EqDraft {
  bands: Band[];
  automatic: boolean;
  preamp: number;
  live: boolean;
  pending: boolean;
}
interface Settings {
  output?: Pick<ControlStatus, 'attenuation' | 'mute'>;
  sound?: SoundSettings;
  eqDraft?: EqDraft;
}

export const settingsKey = (info: DeviceInfo) => `antdec-settings:${info.protocol}:${info.modelId}:${info.model}`;
export function readSettings(key: string): Settings {
  try { return JSON.parse(localStorage.getItem(key) || '{}') as Settings; }
  catch { return {}; }
}
export function writeSettings(key: string, update: Partial<Settings>): boolean {
  try { localStorage.setItem(key, JSON.stringify({ ...readSettings(key), ...update })); return true; }
  catch { return false; }
}
