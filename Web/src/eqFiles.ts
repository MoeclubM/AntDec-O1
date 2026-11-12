import type { Band } from './transports';

export interface EqFile {
  format: string;
  bands: Band[];
  preamp: number;
  automatic: boolean;
  warnings: string[];
}
const filterTypes: Record<string, number> = {
  OFF: 0, NONE: 0, PK: 1, PEQ: 1, PEAK: 1, PEAKING: 1,
  LS: 2, LSC: 2, LSQ: 2, LOWSHELF: 2,
  HS: 3, HSC: 3, HSQ: 3, HIGHSHELF: 3,
  LP: 4, LPQ: 4, LOWPASS: 4, HP: 5, HPQ: 5, HIGHPASS: 5,
};
const number = '[+-]?(?:\\d+(?:\\.\\d*)?|\\.\\d+)(?:e[+-]?\\d+)?';
const field = (text: string, name: string, unit = '') => {
  const match = text.match(new RegExp(`\\b${name}\\s+(${number})${unit}`, 'i'));
  return match ? Number(match[1]) : undefined;
};
function filterType(value: unknown): number {
  if (typeof value === 'number' && Number.isInteger(value) && value >= 0 && value <= 5) return value;
  const type = typeof value === 'string' ? filterTypes[value.toUpperCase().replace(/[\s_-]/g, '')] : undefined;
  if (type === undefined) throw new Error(`不支持的滤波类型：${String(value)}`);
  return type;
}
function band(value: Record<string, unknown>): Band {
  const type = filterType(value.type);
  const result = {
    type, enabled: value.enabled !== false && type !== 0,
    frequency: value.frequency ?? value.fc ?? (type === 0 ? 1000 : NaN),
    gain: value.gain ?? (type === 0 || type >= 4 ? 0 : NaN),
    q: value.q ?? (type === 0 || type >= 2 ? Math.SQRT1_2 : NaN),
  };
  for (const [key, min, max] of [['frequency', 20, 40000], ['gain', -18, 18], ['q', .1, 16]] as const) {
    const value = result[key];
    if (typeof value !== 'number' || !Number.isFinite(value) || value < min || value > max)
      throw new Error(`${key === 'frequency' ? '频率' : key === 'gain' ? '增益' : 'Q'}必须在 ${min}～${max} 之间`);
  }
  if (value.enabled !== undefined && typeof value.enabled !== 'boolean') throw new Error('enabled 必须为 true 或 false');
  return result as Band;
}
function finish(format: string, bands: Band[], preamp: unknown = 0, automatic?: unknown): EqFile {
  if (typeof preamp !== 'number' || !Number.isFinite(preamp) || preamp < -36 || preamp > 0)
    throw new Error('前级增益必须在 −36～0 dB 之间');
  if (automatic !== undefined && typeof automatic !== 'boolean') throw new Error('automatic 必须为 true 或 false');
  const warnings: string[] = [];
  const roundedPreamp=Math.round(preamp*100)/100;
  if (roundedPreamp === 0 && automatic !== true) warnings.push('设备以前级 0 dB 表示自动余量，将按自动余量应用。');
  if (bands.some(b => b.q < .15)) warnings.push('Q 小于 0.15 的频段会按 0.15 处理。');
  const rounded = bands.map(b => ({ ...b, frequency: Math.round(b.frequency),
    gain: Math.round(b.gain * 100) / 100, q: Math.round(b.q * 100) / 100 }));
  if (rounded.some((b, i) => b.frequency !== bands[i].frequency || b.gain !== bands[i].gain || b.q !== bands[i].q))
    warnings.push('频率按 1 Hz 取整，增益和 Q 保留两位小数。');
  return { format, bands: rounded.filter(b => b.type !== 0), preamp: roundedPreamp,
    automatic: roundedPreamp === 0 || automatic === true, warnings };
}
function parseJson(text: string): EqFile {
  const value = JSON.parse(text);
  if (!value || typeof value !== 'object' || Array.isArray(value)) throw new Error('JSON 必须为 EQ 配置对象');
  const filters = value.bands ?? value.filters;
  if (value.format !== undefined && value.format !== 'antdec-eq') throw new Error('不支持此 JSON 配置格式');
  if (value.version !== undefined && value.version !== 1) throw new Error('不支持此 EQ 文件版本');
  if (!Array.isArray(filters)) throw new Error('JSON 必须包含 bands 或 filters 数组');
  return finish('JSON', filters.map((item, index) => {
    try {
      if (!item || typeof item !== 'object' || Array.isArray(item)) throw new Error('频段参数无效');
      return band(item);
    } catch (e) { throw new Error(`频段 ${index + 1}：${(e as Error).message}`); }
  }), value.preamp ?? 0, value.automatic);
}
function parseText(text: string): EqFile {
  const bands: Band[] = [];
  let preamp = 0, recognized = false;
  for (const [index, original] of text.split(/\r?\n/).entries()) {
    const line = original.replace(/#.*/, '').trim();
    if (!line) continue;
    try {
      if (/^GraphicEQ\s*:/i.test(line)) throw new Error('GraphicEQ 曲线无法直接用于参数均衡器，请导出 ParametricEQ 文件');
      if (/^Channel\s*:/i.test(line)) {
        if (!/^Channel\s*:\s*(all|l\s+r|r\s+l)\s*$/i.test(line)) throw new Error('设备使用左右共用 EQ，无法导入独立声道配置');
        continue;
      }
      if (/^(Include|Convolution|Copy|Device|Stage|VSTPlugin|Delay|If|Else|EndIf)\s*:/i.test(line))
        throw new Error('文件包含设备不支持的处理指令，请仅导出参数 EQ');
      if (/^Preamp\s*:/i.test(line)) {
        const match = line.match(new RegExp(`^Preamp\\s*:\\s*(${number})\\s*dB\\s*$`, 'i'));
        if (!match) throw new Error('前级增益格式无效');
        preamp = Number(match[1]); recognized = true; continue;
      }
      if (!/^Filter\s*\d*\s*:/i.test(line)) {
        if (/^[\w ]+\s*:/.test(line) && !/^(Equaliser|Equalizer|Notes|Date|Dated|Target level)\s*:/i.test(line))
          throw new Error('文件包含设备不支持的处理指令，请仅导出参数 EQ');
        continue;
      }
      const match = line.match(/^Filter\s*\d*\s*:\s*(ON|OFF)\s+(\S+)\s*(.*)$/i);
      if (!match) throw new Error('频段格式无效');
      recognized = true;
      const type = filterType(match[2]);
      if (type === 0) continue;
      const frequency = match[3].match(new RegExp(`\\bFc\\s+(${number})\\s*(k?Hz)\\b`, 'i'));
      if (!frequency) throw new Error('缺少 Fc 频率');
      const extra=match[3].replace(frequency[0],'')
        .replace(new RegExp(`\\bGain\\s+${number}\\s*dB\\b`,'i'),'')
        .replace(new RegExp(`\\bQ\\s+${number}(?=\\s|$)`,'i'),'').trim();
      if (extra) throw new Error(`不支持的滤波参数：${extra}`);
      bands.push(band({ type, enabled: match[1].toUpperCase() === 'ON',
        frequency: Number(frequency[1]) * (/^k/i.test(frequency[2]) ? 1000 : 1),
        gain: field(match[3], 'Gain', '\\s*dB\\b'), q: field(match[3], 'Q', '(?=\\s|$)') }));
    } catch (e) { throw new Error(`第 ${index + 1} 行：${(e as Error).message}`); }
  }
  if (!recognized) throw new Error('未找到参数 EQ，请选择 Equalizer APO、AutoEQ、REW 参数文件或 AntDec JSON');
  return finish('Equalizer APO / AutoEQ / REW', bands, preamp);
}
export function parseEqFile(text: string): EqFile {
  const content = text.replace(/^\uFEFF/, '').trim();
  return /^[{[]/.test(content) ? parseJson(content) : parseText(content);
}
export function decodeEqFile(buffer: ArrayBuffer): string {
  const bytes = new Uint8Array(buffer);
  const encoding = bytes[0] === 0xff && bytes[1] === 0xfe ? 'utf-16le'
    : bytes[0] === 0xfe && bytes[1] === 0xff ? 'utf-16be' : 'utf-8';
  return new TextDecoder(encoding).decode(buffer);
}
export function exportEqFile(bands: Band[], preamp: number, automatic: boolean): string {
  return JSON.stringify({ format: 'antdec-eq', version: 1, automatic, preamp,
    bands: bands.filter(b => b.type !== 0) }, null, 2) + '\n';
}
