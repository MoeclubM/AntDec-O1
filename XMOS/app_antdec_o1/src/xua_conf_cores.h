#ifdef __XC__
on tile[0]: Stm32LinkTask(c_tile_bridge);
on tile[1]: AudioCtrlTask(c_tile_bridge);
on tile[1]: DspWorkerTask();
#endif
