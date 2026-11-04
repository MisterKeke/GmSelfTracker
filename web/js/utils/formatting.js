export const FIELD_TYPES = [
  ['text','Text'],['long_text','Long text'],['integer','Integer'],['decimal','Decimal number'],['percentage','Percentage'],['boolean','Checkbox'],['select','Select'],['date','Date'],['progress','Progress'],['reference','Single object reference'],['references','Multiple object references'],['formula','Calculated formula']
];

export function sectionDisplayConfig(section){const config=section?.display_config||{};return config&&typeof config==='object'?config:{};}
export function readSectionDisplayConfig(form){
  const value=(name)=>form.elements.namedItem(name)?.value||'';
  const selected=(name)=>[...form.querySelectorAll(`input[name="${name}"]:checked`)].map((input)=>input.value);
  return {visible_field_ids:selected('visible_field_ids'),checklist_field_id:value('checklist_field_id'),progress_field_id:value('progress_field_id'),progress_mode:value('progress_mode')||'field',progress_current_field_ids:selected('progress_current_field_ids'),progress_max_field_ids:selected('progress_max_field_ids'),progress_maximum:Number(value('progress_maximum')||100),progress_relationship_field_id:value('progress_relationship_field_id'),progress_child_field_id:value('progress_child_field_id')};
}
export function typeLabel(type){return FIELD_TYPES.find(([id])=>id===type)?.[1]||type;}
export function prettyValue(raw,field,objects){
  const value=raw;if(value===undefined||value===null||value==='')return '—';
  if(typeof value==='boolean')return value?'Yes':'No';
  if(field.field_type==='progress'&&value&&typeof value==='object'){const current=Number(value.current)||0;const max=Number(value.max)||0;return max?`${formatNumber(current)} / ${formatNumber(max)} (${Math.round(current/max*100)}%)`:'—';}
  if(field.field_type==='reference'||field.field_type==='references'){const ids=Array.isArray(value)?value:[value];const labels=ids.map((id)=>objects.find((item)=>item.id===id)?.name||String(id).slice(0,8));return labels.join(', ')||'—';}
  if(field.field_type==='percentage')return `${formatNumber(value)}%`;
  return typeof value==='object'?JSON.stringify(value):String(value);
}
export function formatNumber(value){const number=Number(value);return Number.isFinite(number)?new Intl.NumberFormat(undefined,{maximumFractionDigits:2}).format(number):'0';}
export function readDefaultValue(raw,fieldType){
  if(['integer','decimal','percentage'].includes(fieldType)){const value=Number(raw);if(!Number.isFinite(value))throw new Error('Default value must be a valid number.');return value;}
  if(fieldType==='boolean'){if(raw!=='true'&&raw!=='false')throw new Error('Boolean default must be true or false.');return raw==='true';}
  if(fieldType==='progress'){try{return JSON.parse(raw);}catch{throw new Error('Progress default must be JSON with current and max values.');}}
  if(fieldType==='select'||fieldType==='text'||fieldType==='long_text'||fieldType==='date')return raw;
  throw new Error('This field type cannot have a default value.');
}
export function formatDefaultValue(value) {
  if(value===undefined||value===null)return '';
  return typeof value==='string'?value:JSON.stringify(value);
}
export function esc(value){return String(value??'').replace(/[&<>"']/g,(char)=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[char]));}

export function iconUploadInput(){return '<label>Upload icon image (optional)<input type="file" name="icon_file" accept="image/png,image/jpeg,image/gif,image/webp,image/bmp"></label><small>Maximum 5 MB; uploading replaces the symbol.</small>';}
export function renderIcon(value){const text=String(value||'✦');return /^(\/api\/assets\/|https?:\/\/)/.test(text)?`<img class="custom-icon" src="${esc(text)}" alt="" loading="lazy" referrerpolicy="no-referrer">`:esc(text);}
