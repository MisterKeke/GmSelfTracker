import { ensureDialog } from '../modal.js';
import { request } from '../api.js';
import { esc, prettyValue } from '../utils/formatting.js';

export function createComparisonEditor({ openForm, state, root }) {
  let subjects=[];
  let presetsByObject=new Map();
  let currentPresetIDs=[];
  let currentResult=null;
  let currentPreferences={};

  async function openObjectComparison(objectIDs=state.compareSelection||[]) {
    const ids=objectIDs.filter(Boolean);
    if(ids.length<2||ids.length>4)throw new Error('Select between two and four objects to compare.');
    const objects=await Promise.all(ids.map(async id=>state.objectCache.get(id)||state.objects.find(object=>object.id===id)||await request(`/api/objects/${encodeURIComponent(id)}`)));
    if(objects.some(object=>object.game_id!==state.game?.id))throw new Error('Choose objects from the current game.');
    if(objects.some(object=>object.object_type_id!==objects[0].object_type_id))throw new Error('Compare objects that use the same type and field schema.');
    subjects=objects;presetsByObject=new Map();
    for(const object of objects)presetsByObject.set(object.id,(await request(`/api/objects/${encodeURIComponent(object.id)}/presets`)).items||[]);
    currentPresetIDs=objects.map(()=> '');currentPreferences={};
    currentResult=await loadComparison();
    renderDialog();
  }

  async function loadComparison() {
    return request(`/api/games/${encodeURIComponent(state.game.id)}/comparison`,{method:'POST',body:{items:subjects.map((object,index)=>({object_id:object.id,preset_id:currentPresetIDs[index]||''}))}});
  }

  function renderDialog() {
    openForm('OBJECT COMPARISON',`<div class="comparison-result" data-comparison-result>${renderTable(currentResult,currentPreferences)}</div><p class="dialog-help wide">Comparisons use only these selected objects and their shared active field definitions. “Higher is better” and “Lower is better” are optional display preferences; the default is neutral.</p>`,'CLOSE');
    const form=ensureDialog(root).querySelector('#modal-form');form.dataset.form='close-modal';
    form.addEventListener('submit',event=>{event.preventDefault();event.stopPropagation();});
    form.addEventListener('click',event=>{
      const add=event.target.closest('[data-comparison-add-object]');
      if(add)addPresetColumn(Number(add.dataset.comparisonAddObject)).catch(error=>{const output=form.querySelector('[data-comparison-result]');output.insertAdjacentHTML('afterbegin',`<p class="form-error" role="alert">${esc(error.message)}</p>`);});
    });
    form.addEventListener('change',async event=>{
      if(event.target.matches('[data-comparison-preset]')){
        const selects=[...form.querySelectorAll('[data-comparison-preset]')];currentPresetIDs=selects.map(select=>select.value);
        const output=form.querySelector('[data-comparison-result]');output.innerHTML='<p class="query-count" role="status">Recalculating selected configurations…</p>';
        try{currentResult=await loadComparison();output.innerHTML=renderTable(currentResult,currentPreferences);}
        catch(error){output.innerHTML=`<p class="form-error" role="alert">${esc(error.message)}</p>`;}
      } else if(event.target.matches('[data-comparison-preference]')){
        currentPreferences=Object.fromEntries([...form.querySelectorAll('[data-comparison-preference]')].map(select=>[select.dataset.fieldId,select.value]));
        const output=form.querySelector('[data-comparison-result]');output.innerHTML=renderTable(currentResult,currentPreferences);
      }
    });
  }

  async function addPresetColumn(index) {
    if(subjects.length>=4)throw new Error('Compare up to four configurations at a time.');
    const object=subjects[index];if(!object)throw new Error('That comparison object is no longer available.');
    const used=currentPresetIDs.filter((_,subjectIndex)=>subjects[subjectIndex].id===object.id);
    const preset=(presetsByObject.get(object.id)||[]).find(item=>!used.includes(item.id));
    if(!preset)throw new Error('Save another preset for this object before adding another configuration.');
    subjects.push(object);currentPresetIDs.push(preset.id);
    currentResult=await loadComparison();renderDialog();
  }

  function renderTable(result,preferences={}) {
    if(!result?.subjects?.length)return '<p class="empty-state">No comparison data is available.</p>';
    const columns=result.subjects;const fields=(columns[0].fields||[]).filter(field=>!field.archived);
    const head=`<thead><tr><th>Characteristic</th>${columns.map((subject,index)=>{const presets=presetsByObject.get(subject.object.id)||[];const used=currentPresetIDs.filter((_,subjectIndex)=>subjects[subjectIndex].id===subject.object.id);const canAdd=columns.length<4&&presets.some(preset=>!used.includes(preset.id));return `<th><div class="comparison-subject"><span>${esc(subject.object.name)}</span>${subject.object.image?`<img src="${esc(subject.object.image)}" loading="lazy" alt="">`:''}<small>${esc(subject.object.type_name)} · ${esc(subject.preset_name||'Base object')}</small><label>Configuration<select data-comparison-preset data-index="${index}"><option value="" ${!currentPresetIDs[index]?'selected':''}>Base object</option>${presets.map(preset=>`<option value="${esc(preset.id)}" ${currentPresetIDs[index]===preset.id?'selected':''}>${esc(preset.name)}</option>`).join('')}</select></label>${canAdd?`<button class="pixel-button small" type="button" data-comparison-add-object="${index}">Add another configuration</button>`:''}</div></th>`;}).join('')}</tr></thead>`;
    const rows=fields.map(field=>{
      const preference=preferences[field.id]||'neutral';
      const values=columns.map(subject=>subject.values?.[field.id]);
      const cells=columns.map((subject,index)=>{const error=subject.formula_errors?.[field.id];const label=field.field_type==='reference'||field.field_type==='references'?(subject.reference_names?.[field.id]||[]).join(', ')||'—':error?`⚠ ${error}`:prettyValue(values[index],field,columns.map(item=>item.object));return `<td>${esc(label)}</td>`;}).join('');
      const numeric=['integer','decimal','percentage','formula'].includes(field.field_type)?values.map((value,index)=>({value,index})).filter(item=>typeof item.value==='number'&&Number.isFinite(item.value)):[];
      let summary='';
      if(numeric.length>1){const sorted=[...numeric].sort((a,b)=>a.value-b.value);const low=sorted[0],high=sorted.at(-1),difference=high.value-low.value;const percent=low.value>0?` · ${((difference/Math.abs(low.value))*100).toFixed(1)}% difference from lowest`:' · percent difference unavailable';const preferred=preference==='higher'?columns[high.index].object.name:preference==='lower'?columns[low.index].object.name:'';summary=`<small class="comparison-difference">Difference ${esc(String(difference))}${esc(percent)} · highest ${esc(columns[high.index].object.name)} · lowest ${esc(columns[low.index].object.name)}${preferred?` · preferred by setting: ${esc(preferred)}`:''}</small>`;}
      return `<tr><th scope="row"><strong>${esc(field.name)}</strong><small>${esc(field.field_type)}</small><label class="comparison-preference">Preference<select data-comparison-preference data-field-id="${esc(field.id)}"><option value="neutral" ${preference==='neutral'?'selected':''}>Neutral</option><option value="higher" ${preference==='higher'?'selected':''}>Higher is better</option><option value="lower" ${preference==='lower'?'selected':''}>Lower is better</option></select></label>${summary}</th>${cells}</tr>`;
    }).join('');
    return `<div class="comparison-table-scroll"><table class="comparison-table">${head}<tbody>${rows||'<tr><td colspan="5">No active fields are available.</td></tr>'}</tbody></table></div>`;
  }

  return { openObjectComparison };
}
