import type { Band } from './transports';

function coefficients(b: Band, rate: number): number[] {
  if (!b.enabled || !b.type || b.frequency >= rate * .47 ||
    (b.type <= 3 && Math.abs(b.gain) < .02)) return [1, 0, 0, 1, 0, 0];
  const w = 2*Math.PI*Math.max(15, b.frequency)/rate;
  const c = Math.cos(w), alpha = Math.sin(w)/(2*Math.max(.15, b.q)), a = 10**(b.gain/40);
  const shelf = 2*Math.sqrt(a)*alpha;
  switch (b.type) {
    case 1: return [1+alpha*a,-2*c,1-alpha*a,1+alpha/a,-2*c,1-alpha/a];
    case 2: return [a*((a+1)-(a-1)*c+shelf),2*a*((a-1)-(a+1)*c),a*((a+1)-(a-1)*c-shelf),
      (a+1)+(a-1)*c+shelf,-2*((a-1)+(a+1)*c),(a+1)+(a-1)*c-shelf];
    case 3: return [a*((a+1)+(a-1)*c+shelf),-2*a*((a-1)+(a+1)*c),a*((a+1)+(a-1)*c-shelf),
      (a+1)-(a-1)*c+shelf,2*((a-1)-(a+1)*c),(a+1)-(a-1)*c-shelf];
    case 4: return [(1-c)/2,1-c,(1-c)/2,1+alpha,-2*c,1-alpha];
    case 5: return [(1+c)/2,-(1+c),(1+c)/2,1+alpha,-2*c,1-alpha];
    default: return [1,0,0,1,0,0];
  }
}
function response(f: number, rate: number, c: number[]) {
  const w=2*Math.PI*f/rate, cos=Math.cos(w), sin=Math.sin(w), cos2=Math.cos(2*w), sin2=Math.sin(2*w);
  const num=(c[0]+c[1]*cos+c[2]*cos2)**2+(c[1]*sin+c[2]*sin2)**2;
  const den=(c[3]+c[4]*cos+c[5]*cos2)**2+(c[4]*sin+c[5]*sin2)**2;
  return 10*Math.log10(Math.max(1e-12,num/den));
}
export const graphX = (f: number) => 42+Math.log10(f/20)/Math.log10(2000)*890;
export const graphY = (db: number) => 105-Math.max(-24,Math.min(24,db))*3.5;
export const frequencyAt = (x: number) => 20*2000**((Math.max(42,Math.min(932,x))-42)/890);
export const gainAt = (y: number) => Math.max(-18,Math.min(18,(105-y)/3.5));
export function responsePath(bands: Band[], preamp: number, rate: number) {
  const coeffs=bands.map(b=>coefficients(b,rate));
  return Array.from({length:301}, (_,i)=>{
    const f=20*(Math.min(40000,rate*.499)/20)**(i/300), db=preamp+coeffs.reduce((sum,c)=>sum+response(f,rate,c),0);
    return (i?'L':'M')+graphX(f).toFixed(1)+','+graphY(db).toFixed(1);
  }).join(' ');
}
