import { closeModal, ensureDialog } from '../modal.js';
import { request } from '../api.js';
import { esc, prettyValue } from '../utils/formatting.js';

export function createPresetEditor({ openForm, fieldRow, renderFieldInput, readValues, state, root, toast }) {
  async function openObjectPresets(object) {
    const result=await request(`/api/objects/${encodeURIComponent(object.id)}/presets`);
    state.presetObject=object;state.presetData=result;
    openForm(`OBJECT CONFIGURATIONS · ${object.name}`,managerMarkup(object,result),'CLOSE');
    const form=ensureDialog(root).querySelector('#modal-form');form.dataset.form='preset-manager';
    form.addEventListener('submit',event=>{event.preventDefault();event.stopPropagation();closeModal(ensureDialog(root));});
    const select=form.querySelector('[data-preset-view]');
    const preview=form.querySelector('[data-preset-preview]');
    const draw=()=>{preview.innerHTML=renderEffective(result,object,select.value);};
    select.addEventListener('change',draw);draw();
  }

  function managerMarkup(object,result) {
    const presets=result.items||[];
    const options=presets.map(preset=>`<option value="${esc(preset.id)}">${esc(preset.name)}</option>`).join('');
    const rows=presets.length?presets.map(preset=>`<article class="preset-row"><div><strong>${esc(preset.name)}</strong><p>${esc(preset.description||'No description')}</p><small>${Object.keys(preset.values||{}).length} overridden fields · updated ${esc(new Date(preset.updated_at).toLocaleDateString())}</small></div><div class="toolbar"><button class="pixel-button small" type="button" data-action="edit-object-preset" data-id="${esc(preset.id)}">Edit</button><button class="pixel-button small" type="button" data-action="duplicate-object-preset" data-id="${esc(preset.id)}">Duplicate</button><button class="pixel-button small danger" type="button" data-action="delete-object-preset" data-id="${esc(preset.id)}">Delete</button></div></article>`).join(''):'<p class="empty-state">No saved configurations. Create an empty preset or snapshot the current base values.</p>';
    return `<p class="dialog-help">The selected preset is a view of the base object plus its saved overrides. Switching views never writes to the object’s canonical values; goals continue to use those base values.</p><section class="preset-viewer"><label for="preset-display-select">Displayed configuration</label><select id="preset-display-select" data-preset-view><option value="">Base object values</option>${options}</select><div data-preset-preview class="preset-preview" aria-live="polite"></div></section><div class="toolbar preset-create-actions"><button class="pixel-button gold small" type="button" data-action="new-object-preset" data-mode="empty">＋ Empty Preset</button><button class="pixel-button small" type="button" data-action="new-object-preset" data-mode="base">Copy Base Values</button></div><h3 class="preset-subheading">SAVED CONFIGURATIONS (${presets.length})</h3><div class="preset-list">${rows}</div><input type="hidden" name="object_id" value="${esc(object.id)}">`;
  }

  function renderEffective(result,object,presetID) {
    const preset=(result.items||[]).find(item=>item.id===presetID);
    const values=preset?.effective_values||result.canonical_values||object.values||{};
    const overrides=preset?.values||{};
    const errors=preset?.formula_errors||object.formula_errors||{};
    return `<dl class="preset-value-list">${(result.fields||[]).filter(field=>!field.archived).map(field=>{const raw=values[field.id];const error=errors[field.id];const rendered=error?`⚠ ${error}`:prettyValue(raw,field,state.objects);const overridden=Object.prototype.hasOwnProperty.call(overrides,field.id);return `<div class="preset-value-row"><dt>${esc(field.name)}${overridden?'<small class="preset-override-mark">OVERRIDE</small>':''}</dt><dd>${esc(rendered)}</dd></div>`;}).join('')||'<p>No fields are defined for this object type.</p>'}</dl>`;
  }

  function openPresetForm(object,mode='empty',source=null) {
    const data=state.presetData||{};
    const presets=data.items||[];
    const sourcePreset=typeof source==='string'?presets.find(item=>item.id===source):source;
    const seed=mode==='base'?(data.canonical_values||object.values||{}):mode==='duplicate'||mode==='edit'?(sourcePreset?.values||{}):{};
    const fields=(data.fields||object.fields||[]).filter(field=>!field.archived&&field.field_type!=='formula');
    const controls=fields.map(field=>{
      const overridden=Object.prototype.hasOwnProperty.call(seed,field.id);
      const value=overridden?seed[field.id]:((data.canonical_values||object.values||{})[field.id]);
      return `<section class="preset-field-row ${overridden?'is-overridden':''}"><label class="preset-override-toggle"><input type="checkbox" name="override_field_ids" value="${esc(field.id)}" data-preset-override ${overridden?'checked':''}> Override ${esc(field.name)}</label><div class="preset-field-control">${renderFieldInput(field,value,object)}</div></section>`;
    }).join('');
    const modeText=mode==='base'?'Base values are copied as an explicit snapshot. Later base edits will not change this preset.':mode==='duplicate'?'This starts with a copy of the selected preset’s current overrides.':'Unchecked fields inherit the object’s current canonical values.';
    const editing=mode==='edit'&&Boolean(sourcePreset);
    const body=`${fieldRow('Preset name','name',sourcePreset?.name||(mode==='base'?'Base snapshot':mode==='duplicate'?`${sourcePreset?.name||'Preset'} copy`:''),'text',true,'required maxlength="80"')}${fieldRow('Description','description',sourcePreset?.description||'','text',true,'maxlength="500"')}<p class="dialog-help wide">${esc(modeText)} Formula fields stay calculated from the effective values. Goals continue to evaluate canonical object values.</p><div class="preset-field-editor wide">${controls||'<p>This object has no editable fields yet.</p>'}</div><input type="hidden" name="object_id" value="${esc(object.id)}">${sourcePreset?`<input type="hidden" name="preset_id" value="${esc(sourcePreset.id)}">`:''}`;
    openForm(editing?`EDIT PRESET · ${sourcePreset.name}`:sourcePreset?'DUPLICATE PRESET':mode==='base'?'COPY BASE VALUES':'CREATE EMPTY PRESET',body,editing?'SAVE PRESET':sourcePreset?'CREATE COPY':'CREATE PRESET');
    const form=ensureDialog(root).querySelector('#modal-form');
    form.dataset.form=editing?'edit-object-preset':'create-object-preset';
    for(const section of form.querySelectorAll('.preset-field-row')){
      const toggle=section.querySelector('[data-preset-override]');
      const sync=()=>{section.classList.toggle('is-overridden',toggle.checked);for(const control of section.querySelectorAll('.preset-field-control input,.preset-field-control select,.preset-field-control textarea'))control.disabled=!toggle.checked;};
      toggle.addEventListener('change',sync);sync();
    }
  }

  function activeObject() { return state.presetObject; }
  function currentPreset(id) { return (state.presetData?.items||[]).find(item=>item.id===id); }

  return { openObjectPresets, openPresetForm, activeObject, currentPreset };
}
