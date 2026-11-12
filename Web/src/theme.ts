import { ref, watch } from 'vue';

export const dark = ref(document.documentElement.classList.contains('dark'));
watch(dark, value => {
  document.documentElement.classList.toggle('dark', value);
  try { localStorage.setItem('antdec-theme', value ? 'dark' : 'light'); } catch { /* Storage unavailable. */ }
});
