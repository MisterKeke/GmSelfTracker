import { ensureDialog } from '../modal.js';
import { getItems, request } from '../api.js';
import { esc } from '../utils/formatting.js';

const WIDGET_TYPES=[['recent_objects','Recent objects'],['object_collection','Object collection'],['goals','Goals'],['progress','Progress'],['statistics','Statistics'],['custom_table','Custom table'],['notes','Notes'],['image','Image']];
const DEFAULT_LAYOUT={version:1,widgets:[
  {id:'default-recent',type:'recent_objects',title:'Recently edited',size:'large',config:{limit:6}},
  {id:'default-stats',type:'statistics',title:'Game statistics',size:'medium',config:{}},
  {id:'default-goals',type:'goals',title:'Current goals',size:'medium',config:{limit:5,status:'active'}},
]};

export function createDashboardEditor({ openForm, fieldRow, state, root }) {
  async function openDashboardWidgetForm(existing=null) {
    if(!existing&&(state.dashboardLayout?.widgets?.length||0)>=20)throw new Error('A dashboard can contain at most 20 widgets.');
    const [assetsResult,goalsResult]=await Promise.allSettled([
      getItems('/api/assets'),
      request(`/api/games/${encodeURIComponent(state.game.id)}/goals?limit=100&offset=0&sort=title&direction=asc`),
    ]);
    const assets=assetsResult.status==='fulfilled'?assetsResult.value.filter(item=>!item.pending_deletion):[];
    const goals=goalsResult.status==='fulfilled'?(goalsResult.value.items||[]):[];
    const config=existing?.config||{};
    const typeOptions=WIDGET_TYPES.map(([id,label])=>`<option value="${id}" ${existing?.type===id?'selected':''}>${label}</option>`).join('');
    const typeSelect=`<div class="form-field"><label for="f-widget_type">Widget type</label><select name="widget_type" id="f-widget_type" required>${typeOptions}</select></div>`;
    const sizeSelect=`<div class="form-field"><label for="f-widget_size">Width</label><select name="widget_size" id="f-widget_size">${[['small','Small'],['medium','Medium'],['large','Large']].map(([id,label])=>`<option value="${id}" ${((existing?.size||'medium')===id)?'selected':''}>${label}</option>`).join('')}</select></div>`;
    const types=state.types.filter(type=>!type.archived);
    const fields=types.flatMap(type=>(type.fields||[]).filter(field=>!field.archived).map(field=>({field,type})));
    const body=`${fieldRow('Widget title','widget_title',existing?.title||'','text',true,'required maxlength="80"')}${typeSelect}${sizeSelect}<label class="form-field">Object type<select name="type_id" data-dashboard-type><option value="">No type selected</option>${types.map(type=>`<option value="${esc(type.id)}" ${config.type_id===type.id?'selected':''}>${esc(type.name)}</option>`).join('')}</select></label><label class="form-field">Section<select name="section_id"><option value="">No section selected</option>${state.sections.filter(section=>!section.archived).map(section=>`<option value="${esc(section.id)}" ${config.section_id===section.id?'selected':''}>${esc(section.name)}</option>`).join('')}</select></label><label class="form-field wide">Visible fields<select name="field_ids" multiple size="6">${fields.map(({field,type})=>`<option data-object-type="${esc(type.id)}" value="${esc(field.id)}" ${(config.field_ids||[]).includes(field.id)?'selected':''}>${esc(type.name)} · ${esc(field.name)} (${esc(field.field_type)})</option>`).join('')}</select><small>Used by custom tables and progress widgets. Choose a type to filter fields to that schema; use Ctrl or Command to select several fields.</small></label><label class="form-field wide">Selected goals<select name="goal_ids" multiple size="5">${goals.map(goal=>`<option value="${esc(goal.id)}" ${(config.goal_ids||[]).includes(goal.id)?'selected':''}>${esc(goal.title)}</option>`).join('')}</select><small>Leave empty to show goals matching the filter.</small></label><label class="form-field">Goal status<select name="status">${[['all','All'],['active','Active'],['completed','Completed']].map(([id,label])=>`<option value="${id}" ${(config.status||'active')===id?'selected':''}>${label}</option>`).join('')}</select></label><label class="form-field">Sort order<select name="sort">${[['updated_at','Recently updated'],['created_at','Recently created'],['name','Name']].map(([id,label])=>`<option value="${id}" ${(config.sort||'updated_at')===id?'selected':''}>${label}</option>`).join('')}</select></label><label class="form-field">Direction<select name="direction"><option value="desc" ${(config.direction||'desc')==='desc'?'selected':''}>Descending</option><option value="asc" ${config.direction==='asc'?'selected':''}>Ascending</option></select></label>${fieldRow('Items to show','limit',config.limit||6,'number',false,'min="1" max="30" step="1"')}
      <div class="form-field wide"><label for="f-note_text">Notes text</label><textarea id="f-note_text" name="note_text" maxlength="4000">${esc(config.text||'')}</textarea><small>Stored as plain text. Markup and scripts are displayed as text.</small></div><label class="form-field wide">Uploaded image<select name="asset_id"><option value="">Choose an account image</option>${assets.map(asset=>`<option value="${esc(asset.id)}" ${config.asset_id===asset.id?'selected':''}>${esc(asset.id)} · ${Math.round(Number(asset.size_bytes||0)/1024)} KB</option>`).join('')}</select>${assetsResult.status==='rejected'?`<small role="alert">Images could not be loaded: ${esc(assetsResult.reason?.message||'request failed')}</small>`:''}</label>${goalsResult.status==='rejected'?`<p class="dialog-help wide">Goal picker unavailable: ${esc(goalsResult.reason?.message||'request failed')}</p>`:''}${existing?`<input type="hidden" name="widget_id" value="${esc(existing.id)}">`:''}`;
    openForm(existing?'CONFIGURE DASHBOARD WIDGET':'ADD DASHBOARD WIDGET',body,existing?'SAVE WIDGET':'ADD WIDGET');
    const form=ensureDialog(root).querySelector('#modal-form');form.dataset.form='dashboard-widget';
    const typeFieldSelect=form.elements.namedItem('type_id'),fieldSelect=form.elements.namedItem('field_ids');
    const filterFields=()=>{const typeID=typeFieldSelect.value;for(const option of fieldSelect.options){const unavailable=Boolean(typeID&&option.dataset.objectType!==typeID);option.disabled=unavailable;if(unavailable)option.selected=false;}};
    typeFieldSelect.addEventListener('change',filterFields);filterFields();
  }

  function readDashboardWidget(form) {
    const data=new FormData(form),values=Object.fromEntries(data.entries());
    const config={};
    for(const key of ['type_id','section_id','status','sort','direction','asset_id'])if(values[key])config[key]=values[key];
    const fieldIDs=[...form.elements.field_ids.selectedOptions].map(option=>option.value);
    const goalIDs=[...form.elements.goal_ids.selectedOptions].map(option=>option.value);
    if(fieldIDs.length)config.field_ids=fieldIDs;if(goalIDs.length)config.goal_ids=goalIDs;
    if(values.note_text)config.text=values.note_text;
    if(values.limit)config.limit=Number(values.limit);
    const type=values.widget_type;
    if(type==='notes')config.text=values.note_text||'';
    if(type==='image')config.asset_id=values.asset_id||'';
    return {id:values.widget_id||newWidgetID(),type,title:values.widget_title.trim(),size:values.widget_size,config};
  }

  function defaultDashboard() { return typeof structuredClone==='function'?structuredClone(DEFAULT_LAYOUT):JSON.parse(JSON.stringify(DEFAULT_LAYOUT)); }
  function newWidgetID() { return globalThis.crypto?.randomUUID?.()||`widget-${Date.now()}-${Math.random().toString(36).slice(2,9)}`; }
  function widgetTypes() { return WIDGET_TYPES; }
  return { openDashboardWidgetForm, readDashboardWidget, defaultDashboard, widgetTypes };
}
