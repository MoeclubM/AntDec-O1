export interface ControlStatus {
  online: boolean; filter: number; attenuation: number; power: string; fault: number;
  amp: number; mute: number; dac: number; pg1: number; pg2: number; vdda: number;
  token: number; ack: number; result: number;
  playSeconds: number; storageFault: boolean; filterPersistent: boolean; settingsPending: boolean; advancedOutput: boolean;
  configSyncing: boolean; configSyncFailed: boolean;
}
export interface OutputSettings {
  supported: boolean; filter: number; balance: number; startup: number; ceiling: number;
  fadeUp: number; fadeDown: number; automuteLevel: number; automuteTime: number;
  invert: number; automuteEnabled: boolean; ready: boolean; managed: boolean; revision: number;
}
export const outputOperations = { balance: 6, startup: 7, ceiling: 8, fadeUp: 9, fadeDown: 10,
  automuteLevel: 11, automuteTime: 12, invert: 13, automuteEnabled: 14 } as const;
export type OutputSetting = keyof typeof outputOperations;
export interface AudioStatus {
  usbMode: number; dsp: number; crossfeed: boolean; format: number;
  rate: number; bits: number; online: boolean; playing: boolean; preamp: number;
  playSeconds: number; saveStatus: number; automaticSave: boolean; managementFault: boolean;
}
export interface Band { frequency: number; gain: number; q: number; type: number; enabled: boolean }
export interface Meters { valid: boolean; left: number; right: number }
export interface DeviceInfo {
  protocol: number; model: string; modelId: number; features: number;
  maxPcmRate: number; maxDspRate: number; bands: number;
}

export class AudioControl {
  device?: USBDevice;
  private iface = -1;
  private queue: Promise<unknown> = Promise.resolve();
  private managed = false;
  get connected() { return !!this.device?.opened; }
  async connect() {
    const granted = (await navigator.usb.getDevices()).filter(d => d.manufacturerName === 'AntDec'
      || d.productName?.startsWith('AntDec') || d.vendorId === 0x20b1 && d.productId === 0x301f
      || d.vendorId === 0x2972 && d.productId === 0x0093);
    this.device = granted.length === 1 ? granted[0] : await navigator.usb.requestDevice({ filters: [
      { vendorId: 0x20b1 }, { vendorId: 0x2972, productId: 0x0093 }
    ] });
    try {
      await this.device.open();
      if (!this.device.configuration) await this.device.selectConfiguration(1);
      const intf = this.device.configuration!.interfaces.find(i => i.alternates.some(a =>
        a.interfaceClass === 0xfe && a.interfaceSubclass === 1 && a.interfaceProtocol === 1));
      if (!intf) throw new Error('设备没有提供 USB 控制接口');
      this.iface = intf.interfaceNumber;
      await this.device.claimInterface(this.iface);
      return await this.status();
    } catch (e) {
      await this.disconnect();
      throw e;
    }
  }
  private transfer<T>(fn: (d: USBDevice) => Promise<T>): Promise<T> {
    const device = this.device;
    const run = this.queue.then(() => {
      if (!device?.opened || this.device !== device) throw new Error('音频板未连接');
      return fn(device);
    });
    this.queue = run.catch(() => {});
    return run;
  }
  read(request: number, length: number, index = 0) {
    return this.transfer(async d => {
      const r = await d.controlTransferIn({ requestType: 'vendor', recipient: 'device', request, value: 0, index }, length);
      if (r.status !== 'ok' || !r.data || r.data.byteLength !== length) throw new Error('设备读取失败，请检查固件版本');
      return r.data;
    });
  }
  write(request: number, value = 0, index = 0, data?: Uint8Array) {
    return this.transfer(async d => {
      const payload = data === undefined ? undefined : new Uint8Array(data).buffer;
      const r = await d.controlTransferOut({ requestType: 'vendor', recipient: 'device', request, value: value & 0xffff, index }, payload);
      if (r.status !== 'ok') throw new Error('设备拒绝设置，请检查参数和连接状态');
    });
  }
  async status(): Promise<AudioStatus> {
    const d = await this.read(0xa0, 28);
    if (d.getUint32(0) !== 0x41444f31) throw new Error('设备状态协议不匹配');
    return { usbMode: d.getUint8(4), dsp: d.getUint8(5), crossfeed: !!d.getUint8(6),
      format: d.getUint8(7), rate: d.getUint32(8, true), bits: d.getUint8(12),
      online: !!(d.getUint8(13) & 1), playing: !!(d.getUint8(13) & 2), preamp: d.getInt16(14, true) / 100,
      playSeconds: d.getUint32(20, true), saveStatus: d.getUint8(24),
      automaticSave: !!(d.getUint8(25) & 1), managementFault: !!(d.getUint8(25) & 12) };
  }
  async info(): Promise<DeviceInfo> {
    const d = await this.read(0xad, 32);
    if (d.getUint32(0) !== 0x41444331) throw new Error('此设备不支持 AntDec 控制协议');
    this.managed = !!(d.getUint16(6, true) & 128);
    return { protocol: d.getUint8(4), modelId: d.getUint8(5), features: d.getUint16(6, true),
      maxPcmRate: d.getUint32(8, true), maxDspRate: d.getUint32(12, true), bands: d.getUint8(16),
      model: new TextDecoder().decode(new Uint8Array(d.buffer, d.byteOffset + 20, 12)).replace(/\0.*$/, '') };
  }
  async controller(): Promise<ControlStatus> {
    const d = await this.read(0xab, 24);
    if (d.getUint32(0) !== 0x43544c31) throw new Error('请更新 XMOS 固件以读取控制板状态');
    const flags = d.getUint8(5);
    const powers = ['WAIT_POWER', 'AVDD_SETTLE', 'VCCA_SETTLE', 'WAIT_PG',
      'DAC_RESET', 'WAIT_DAC', 'AMP_SETTLE', 'UNMUTE', 'READY', 'FAULT'];
    return { online: !!(d.getUint8(4) & 1), filter: flags & 7, attenuation: d.getUint8(6),
      power: powers[d.getUint8(7)] ?? 'FAULT', fault: d.getUint8(8),
      dac: (flags >> 3) & 1, amp: (flags >> 4) & 1, mute: (flags >> 5) & 1,
      pg1: d.getUint16(10, true), pg2: d.getUint16(12, true), vdda: d.getUint16(14, true),
      token: d.getUint8(16), ack: d.getUint8(17), result: d.getUint8(18),
      playSeconds: d.getUint32(20, true), storageFault: !!(d.getUint8(9) & 1),
      filterPersistent: !!(d.getUint8(9) & 2), settingsPending: !!(d.getUint8(9) & 4),
      advancedOutput: !!(d.getUint8(9) & 8), configSyncing: !!(d.getUint8(4) & 4), configSyncFailed: !!(d.getUint8(4) & 8) };
  }
  async outputSettings(): Promise<OutputSettings> {
    const d = await this.read(0xae, this.managed ? 18 : 16);
    if (d.getUint32(0) !== 0x4f555431) throw new Error('输出设置协议不匹配');
    return { supported: !!(d.getUint8(4) & 2) && d.getUint8(5) === 1,
      filter: d.getUint8(6), balance: d.getInt8(7), startup: d.getUint8(8), ceiling: d.getUint8(9),
      fadeUp: d.getUint8(10), fadeDown: d.getUint8(11), automuteLevel: d.getUint8(12),
      automuteTime: d.getUint8(13), invert: d.getUint8(14), automuteEnabled: !!d.getUint8(15),
      ready: !this.managed || !!(d.getUint8(4) & 4), managed: !!(d.getUint8(4) & 16),
      revision: this.managed ? d.getUint16(16, true) : 0 };
  }
  async setController(op: number, value: number): Promise<ControlStatus> {
    await this.write(0xac, op >= 6 ? value & 255 : value, op);
    let s = await this.controller();
    const token = s.token;
    const deadline = Date.now() + 2500;
    while (Date.now() < deadline) {
      if (s.ack === token && (s.result === 2 || s.result === 3)) {
        if (s.result === 3) throw new Error('控制板未能执行设置，请检查解码状态');
        let actual = op === 1 ? s.filter : op === 2 ? s.attenuation : op === 5 ? s.playSeconds : s.mute;
        if (op >= 6) {
          const field = (Object.keys(outputOperations) as OutputSetting[]).find(key => outputOperations[key] === op);
          const output = await this.outputSettings();
          if (!field || !output.supported) throw new Error('当前固件不支持输出设置');
          actual = Number(output[field]);
        }
        if (actual !== value) throw new Error('控制板回读不一致，请重新应用');
        return s;
      }
      if (s.token === token && s.result === 4) throw new Error('控制板响应超时，请检查板间连接');
      await new Promise(resolve => setTimeout(resolve, 80));
      s = await this.controller();
    }
    throw new Error('等待控制板确认超时');
  }
  async meters(): Promise<Meters> {
    const d = await this.read(0xaa, 8);
    if (d.getUint32(0) !== 0x4d545231) throw new Error('电平数据不可用');
    return { valid: !!d.getUint8(4), left: d.getUint8(5), right: d.getUint8(6) };
  }
  async bands(count = 10): Promise<Band[]> {
    const result: Band[] = [];
    for (let i = 0; i < count; i++) {
      const d = await this.read(0xa5, 8, i);
      result.push({ frequency: d.getUint16(0, true), gain: d.getInt16(2, true) / 100,
        q: d.getUint16(4, true) / 100, type: d.getUint8(6), enabled: !!d.getUint8(7) });
    }
    return result;
  }
  async setBand(index: number, band: Band) {
    const data = new Uint8Array(8), d = new DataView(data.buffer);
    d.setUint16(0, band.frequency, true); d.setInt16(2, Math.round(band.gain * 100), true);
    d.setUint16(4, Math.round(band.q * 100), true); d.setUint8(6, band.type); d.setUint8(7, +band.enabled);
    await this.write(0xa4, 0, index, data);
  }
  async disconnect() {
    const d = this.device;
    this.device = undefined;
    if (d?.opened) {
      try { if (this.iface >= 0) await d.releaseInterface(this.iface); } catch { /* Unplugged. */ }
      try { await d.close(); } catch { /* Unplugged. */ }
    }
    this.iface = -1;
  }
}
