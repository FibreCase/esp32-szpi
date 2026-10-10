import './style.css';
import setupHtml from '../../components/szpi_wifi/assets/portal.html?raw';

const app=document.getElementById('app');
const settingPage=location.pathname==='/settings';
let token='',timer,editing=false,saving=false,lastSettings={};
const hostnameValid=value=>/^[A-Za-z0-9](?:[A-Za-z0-9-]{0,30}[A-Za-z0-9])?$/.test(value);
const $=id=>document.getElementById(id);
const text=(id,value)=>{if($(id))$(id).textContent=value};
const memory=bytes=>bytes==null?'—':(bytes/1048576).toFixed(1);
const cameraStates=['Unavailable','Stopped','Starting','Running','Stopping','Error'];
const storageStates=['Not initialized','No card','No filesystem','Ready','Busy','Formatting','Error'];
async function api(path,body){
  const response=await fetch(path,{method:body?'POST':'GET',cache:'no-store',headers:body?{'Content-Type':'application/json','X-Control-Token':token}:{},body:body?JSON.stringify(body):undefined});
  if(!response.ok)throw new Error(response.status===403?'Session expired. Reload this page.':response.status===409?'Device busy. Please try again.':'Device unavailable. Please try again.');
  return response.json();
}
function notice(message,error=false){text('notice',message);$('notice')?.classList.toggle('error',error)}
function dashboard(){return `<div class="grid">
  <article class="card"><div class="name">UPTIME</div><div class="metric" id="uptime">—</div><div class="detail">Time since last boot</div></article>
  <article class="card"><div class="name">FREE INTERNAL MEMORY</div><div class="metric"><span id="heap">—</span> <span class="unit">MB</span></div><div class="detail">Internal RAM available</div></article>
  <article class="card"><div class="name">FREE PSRAM</div><div class="metric"><span id="psram">—</span> <span class="unit">MB</span></div><div class="detail">External memory available</div></article>
  <article class="card wide"><h2>Network</h2><div class="rows"><div class="row"><span>Connected network</span><span id="ssid">—</span></div><div class="row"><span>IP address</span><code id="ip">—</code></div><div class="row"><span>Signal strength</span><span id="rssi">—</span></div></div></article>
  <article class="card"><h2>Camera</h2><div class="metric section" id="camera">—</div><div class="detail" id="camera-fps">GC2145 · 320 × 240</div></article>
  <article class="card wide"><h2>Device</h2><div class="rows"><div class="row"><span>Hostname</span><span id="hostname">—</span></div><div class="row"><span>Firmware</span><span id="version">—</span></div><div class="row"><span>Running slot</span><code id="slot">—</code></div></div></article>
  <article class="card"><h2>Storage</h2><div class="metric section" id="storage">—</div><div class="detail" id="storage-size">Waiting for device</div></article></div>`}
function settings(){return `<form id="settings-form" class="card settings">
  <h2>Device settings</h2><p class="muted">Changes are saved on your device.</p>
  <div class="setting"><label for="hostname-setting">Hostname</label><input id="hostname-setting" type="text" maxlength="32" autocomplete="off" spellcheck="false" disabled><div class="hint">1–32 letters, digits or hyphens. Takes effect after restart.</div><div class="hint" id="hostname-state"></div></div>
  ${[['brightness','Display brightness',10],['speaker','Speaker volume',0],['microphone','Microphone gain',0]].map(([id,title,min])=>`<div class="setting"><div class="setting-title"><label for="${id}">${title}</label><output id="${id}-value">—</output></div><input id="${id}" type="range" min="${min}" max="100" value="${min}" disabled><div class="hint">${min}–100%</div></div>`).join('')}
  <div class="settings-footer"><button class="save" id="save" disabled>Save changes</button><span class="hint" id="save-state">Waiting for device</span></div></form>`}
function render(){
  app.innerHTML=`<header><a class="brand" href="/"><span class="mark">S</span>SZ-PI</a><nav aria-label="Main"><a href="/" class="${!settingPage?'active':''}">Overview</a><a href="/settings" class="${settingPage?'active':''}">Settings</a></nav></header>
  <main><div class="intro"><div><div class="eyebrow">Device control panel</div><h1>${settingPage?'Settings':'Overview'}</h1><p class="muted">${settingPage?'Keep your display and audio just right.':'A live view of your SZ-PI.'}</p></div><span id="connection" class="badge">Connecting</span></div>${settingPage?settings():dashboard()}<footer><span id="notice" class="notice" role="status" aria-live="polite"></span><span id="updated">Waiting for first update</span></footer></main>`;
  if(settingPage){
    for(const id of ['brightness','speaker','microphone'])$(id).addEventListener('input',()=>{editing=true;text(id+'-value',$(id).value+'%');text('save-state','Unsaved changes')});
    $('hostname-setting').addEventListener('input',()=>{editing=true;text('save-state','Unsaved changes')});
    $('settings-form').addEventListener('submit',saveSettings);
  }
}
async function saveSettings(event){
  event.preventDefault();if(saving)return;
  const hostname=$('hostname-setting').value;
  if(lastSettings.hostname!=null&&!hostnameValid(hostname)){notice('Use 1–32 letters, digits or hyphens, with no leading or trailing hyphen.',true);return}
  saving=true;$('save').disabled=true;$('hostname-setting').disabled=true;
  for(const id of ['brightness','speaker','microphone'])$(id).disabled=true;
  try{
    for(const id of ['brightness','speaker','microphone']){
      const value=Number($(id).value);
      if(lastSettings[id]!=null&&value!==lastSettings[id])await api('/api/settings',{key:id,value});
    }
    if(lastSettings.hostname!=null&&hostname!==lastSettings.hostname)await api('/api/settings',{key:'hostname',value:hostname});
    editing=false;notice('Changes submitted. The device will apply and save them.');text('save-state','Submitted');
  }catch(error){notice(error.message,true);text('save-state','Please retry')}
  finally{saving=false;await refresh()}
}
async function refresh(){
  if(saving)return;
  try{
    const s=await api('/api/device');lastSettings=s.settings;
    text('connection',s.network.online?'Wi-Fi connected':'Wi-Fi offline');$('connection').classList.toggle('online',s.network.online);
    text('uptime',Math.floor(s.uptime_s/3600)+'h '+Math.floor(s.uptime_s%3600/60)+'m');
    text('heap',memory(s.heap_free));text('psram',memory(s.psram_free));
    text('ssid',s.network.ssid||'Not connected');text('ip',s.network.ip||'—');text('rssi',s.network.online&&s.network.rssi!==null?s.network.rssi+' dBm':'—');
    text('hostname',s.hostname||'Unavailable');text('version',s.version);text('slot',s.slot);
    text('camera',cameraStates[s.camera.state]||'Unavailable');text('camera-fps','GC2145 · '+(s.camera.fps_milli/1000).toFixed(1)+' FPS');
    text('storage',storageStates[s.storage.state]||'Unavailable');text('storage-size',s.storage.state===3?memory(s.storage.free_bytes/1024)+' / '+memory(s.storage.capacity_bytes/1024)+' GB free':'SDMMC · 1-bit');
    if(settingPage){
      for(const id of ['brightness','speaker','microphone']){
        const available=id==='brightness'?s.settings.display_available:s.settings.audio_available;
        $(id).disabled=!available;
        if(!editing&&s.settings[id]!=null){$(id).value=s.settings[id];text(id+'-value',s.settings[id]+'%')}
      }
      $('hostname-setting').disabled=s.settings.hostname==null;
      if(!editing&&s.settings.hostname!=null)$('hostname-setting').value=s.settings.hostname;
      text('hostname-state',s.settings.hostname!=null&&s.settings.hostname!==s.hostname?'Restart required. Currently using '+s.hostname+'.':'');
      $('save').disabled=!token||!(s.settings.display_available||s.settings.audio_available||s.settings.hostname!=null);
      if(!editing)text('save-state','Settings up to date');
    }
    text('updated','Updated '+new Date().toLocaleTimeString());
  }catch(error){text('connection','Device unavailable');$('connection').classList.remove('online');notice(error.message,true);if(settingPage)$('save').disabled=true}
}
if(location.pathname==='/setup'){
  const doc=new DOMParser().parseFromString(setupHtml,'text/html');
  const style=document.createElement('style');style.textContent=doc.querySelector('style').textContent;document.head.append(style);
  app.replaceChildren(doc.querySelector('main'));
  const script=document.createElement('script');script.textContent=doc.querySelector('script').textContent;document.body.append(script);
  document.title='SZ-PI Wi-Fi setup';
}else{
  render();
  api('/api/control-session').then(s=>{token=s.token;return refresh()}).catch(error=>notice(error.message,true));
  timer=setInterval(refresh,3000);
  addEventListener('pagehide',()=>clearInterval(timer),{once:true});
}
