import { request } from '../api.js';
import { esc, typeLabel, sectionDisplayConfig } from '../utils/formatting.js';
import { FILTER_OPERATORS, OPERATOR_LABELS, queryObjects } from '../utils/object-query.js';

export function createObjectBrowser({state,root,render,toast,run}) {
  const currentSection=()=>state.panel==='section'?state.sections.find(item=>item.id===state.sectionId):null;
  const key=section=>`${state.game.id}:${section?.id||'library'}`;
  function getObjectView(section=null) {
    const id=key(section);
    if(!state.views.has(id))state.views.set(id,structuredClone(sectionDisplayConfig(section).object_view||{search:'',type_id:'',mode:'all',conditions:[],sort:'name',direction:'asc'}));
    return state.views.get(id);
  }
  function filterObjects(objects,section=null) { return queryObjects(objects,getObjectView(section)); }
  function renderObjectControls(section=null) {
    const view=getObjectView(section),type=state.types.find(item=>item.id===view.type_id&&!item.archived);
    const fields=(type?.fields||[]).filter(field=>!field.archived);
    const sortFields=fields.filter(field=>!['reference','references','long_text'].includes(field.field_type));
    return `<section class="object-browser light-panel" aria-label="Object search and filters"><div class="library-filters"><label>Search objects<input type="search" data-view-search value="${esc(view.search||'')}" placeholder="Name, type or description"></label><label>Object type<select data-view-setting="type_id"><option value="">All types</option>${state.types.filter(item=>!item.archived).map(item=>`<option value="${esc(item.id)}" ${item.id===view.type_id?'selected':''}>${esc(item.name)}</option>`).join('')}</select></label><label>Sort by<select data-view-setting="sort">${[['name','Name'],['type_name','Object type'],['created_at','Date created'],['updated_at','Date updated'],...sortFields.map(field=>[field.id,field.name])].map(([id,label])=>`<option value="${esc(id)}" ${view.sort===id?'selected':''}>${esc(label)}</option>`).join('')}</select></label><label>Order<select data-view-setting="direction"><option value="asc" ${view.direction!=='desc'?'selected':''}>Ascending</option><option value="desc" ${view.direction==='desc'?'selected':''}>Descending</option></select></label></div><div class="toolbar"><label>Match <select data-view-setting="mode"><option value="all" ${view.mode!=='any'?'selected':''}>ALL conditions</option><option value="any" ${view.mode==='any'?'selected':''}>ANY condition</option></select></label><button type="button" class="pixel-button small" data-view-add ${fields.length?'':'disabled'}>＋ Filter</button><button type="button" class="pixel-button small soft" data-view-clear>Clear</button>${section?'<button type="button" class="pixel-button small gold" data-view-save>Save section view</button>':''}</div>${!type?'<p class="dialog-help">Choose an object type to filter or sort by your custom fields.</p>':''}<div class="object-filter-rows">${(view.conditions||[]).map((condition,index)=>{
      const field=fields.find(item=>item.id===condition.field_id);
      return `<div class="object-filter-row" data-filter-index="${index}"><label>Field<select data-filter-setting="field_id"><option value="">${field?'Choose field':'Field unavailable — choose another'}</option>${fields.map(item=>`<option value="${esc(item.id)}" ${item.id===condition.field_id?'selected':''}>${esc(item.name)} · ${esc(typeLabel(item.field_type))}</option>`).join('')}</select></label><label>Comparison<select data-filter-setting="operator">${(FILTER_OPERATORS[field?.field_type]||[]).map(op=>`<option value="${op}" ${op===condition.operator?'selected':''}>${OPERATOR_LABELS[op]}</option>`).join('')}</select></label>${renderTarget(field,condition)}<button type="button" class="pixel-button small soft" data-view-remove="${index}" aria-label="Remove filter ${index+1}">×</button></div>`;
    }).join('')}</div><p class="dialog-help">Empty values do not match filters. Missing sort values appear last. Filters never change object data.</p></section>`;
  }
  function renderTarget(field,condition) {
    if(!field||['checked','unchecked','completed','not_completed'].includes(condition.operator))return '';
    if(field.field_type==='select') {
      const many=condition.operator==='one_of',selected=many?(Array.isArray(condition.value)?condition.value:[]):[condition.value];
      return `<label>Option${many?'s':''}<select data-filter-setting="value" ${many?'multiple size="3"':''}>${(field.options?.choices||[]).map(choice=>`<option value="${esc(choice)}" ${selected.includes(choice)?'selected':''}>${esc(choice)}</option>`).join('')}</select></label>`;
    }
    if(['reference','references'].includes(field.field_type))return `<label>Referenced object<select data-filter-setting="value"><option value="">Choose object</option>${state.objects.filter(object=>!field.options?.allowed_type_id||object.object_type_id===field.options.allowed_type_id).map(object=>`<option value="${esc(object.id)}" ${condition.value===object.id?'selected':''}>${esc(object.name)}</option>`).join('')}</select></label>`;
    const kind=field.field_type==='date'?'date':['text','long_text'].includes(field.field_type)?'text':'number';
    return `<label>${condition.operator==='between'?'From':'Value'}<input data-filter-setting="value" type="${kind}" ${kind==='number'?'step="any"':''} value="${esc(condition.value??'')}"></label>${condition.operator==='between'?`<label>To<input data-filter-setting="value_to" type="${kind}" ${kind==='number'?'step="any"':''} value="${esc(condition.value_to??'')}"></label>`:''}`;
  }
  function refreshObjects() {
    const section=currentSection();
    const objects=section?state.objects.filter(object=>object.section_ids?.includes(section.id)):state.objects;
    // Rendering only the result area keeps focus and typed input intact.
    root.dispatchEvent(new CustomEvent('object-query-changed',{detail:{objects,section}}));
  }
  root.addEventListener('input',event=>{
    if(event.target.matches('[data-view-search]')){getObjectView(currentSection()).search=event.target.value;state.objectPage=0;refreshObjects();}
    if(event.target.matches('input[data-filter-setting]'))updateCondition(event.target,false);
  });
  function updateCondition(input,redraw=true) {
    const view=getObjectView(currentSection()),index=Number(input.closest('[data-filter-index]').dataset.filterIndex),condition=view.conditions[index];
    const field=state.types.flatMap(type=>type.fields||[]).find(item=>item.id===condition.field_id);
    const setting=input.dataset.filterSetting;
    condition[setting]=input.multiple?[...input.selectedOptions].map(option=>option.value):input.type==='number'?(input.value===''?null:Number(input.value)):input.value;
    if(setting==='field_id')resetCondition(condition);
    if(setting==='operator') {
      condition.value=input.value==='one_of'?[]:field?.options?.choices?.[0]??'';condition.value_to=null;
    }
    state.objectPage=0;if(redraw)render();else refreshObjects();
  }
  function resetCondition(condition) {
    const field=state.types.flatMap(type=>type.fields||[]).find(item=>item.id===condition.field_id);
    condition.operator=FILTER_OPERATORS[field?.field_type]?.[0]||'';condition.value=field?.options?.choices?.[0]??'';condition.value_to=null;
  }
  root.addEventListener('change',event=>{
    const input=event.target;
    if(input.dataset.viewSetting) {
      const view=getObjectView(currentSection());view[input.dataset.viewSetting]=input.value;
      if(input.dataset.viewSetting==='type_id'){view.conditions=[];view.sort='name';}
      state.objectPage=0;render();
    }
    if(input.matches('select[data-filter-setting],input[type=date][data-filter-setting]'))updateCondition(input);
  });
  root.addEventListener('click',event=>{
    if(!state.game||!event.target.closest('[data-view-add],[data-view-remove],[data-view-clear],[data-view-save]'))return;
    const view=getObjectView(currentSection());
    if(event.target.closest('[data-view-add]')) {
      if((view.conditions||[]).length>=20){toast('Use no more than 20 filters.',true);return;}
      const field=state.types.find(type=>type.id===view.type_id)?.fields?.find(item=>!item.archived);if(!field)return;
      const condition={field_id:field.id};resetCondition(condition);(view.conditions||=[]).push(condition);render();
    }
    const remove=event.target.closest('[data-view-remove]');if(remove){view.conditions.splice(Number(remove.dataset.viewRemove),1);render();}
    if(event.target.closest('[data-view-clear]')){Object.assign(view,{search:'',type_id:'',mode:'all',conditions:[],sort:'name',direction:'asc'});state.objectPage=0;render();}
    if(event.target.closest('[data-view-save]'))run(async()=>{
      const section=currentSection();if(!section)return;
      const config={...sectionDisplayConfig(section),object_view:structuredClone(view)};
      await request(`/api/sections/${section.id}`,{method:'PATCH',body:{display_config:config}});section.display_config=config;toast('Section view saved.');
    });
  });
  return {getObjectView,filterObjects,renderObjectControls};
}
