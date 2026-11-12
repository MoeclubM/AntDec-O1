import type { DeviceInfo } from './transports';

export const Feature = { dac: 1, eq: 2, meters: 4, timer: 8, usbMode: 16, crossfeed: 32, output: 64, manager: 128 } as const;
export function deviceProfile(info: DeviceInfo) {
  return {
    name: `AntDec ${info.model || info.modelId}`,
    supports: (feature: number) => (info.features & feature) !== 0,
    maxPcmRate: info.maxPcmRate, maxDspRate: info.maxDspRate, bandCount: info.bands,
  };
}
export const filters = ['最小相位', '线性快速 · 切趾', '线性快速', '线性快速 · 低纹波',
  '线性慢速', '最小相位快速', '最小相位慢速', '最小相位慢速 · 低色散'];
export const presets = ['直通', '低音加重', '人声', '暖声', '哈曼预设', '自定义 EQ'];
export const modes = ['标准 UAC2', '山灵标识（实验）', '飞傲标识（实验）', '兼容 UAC1'];
export function duration(seconds = 0) {
  return [Math.floor(seconds / 3600), Math.floor(seconds / 60) % 60, seconds % 60]
    .map(n => String(n).padStart(2, '0')).join(':');
}
