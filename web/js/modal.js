import { esc } from './utils/formatting.js';

const sessions = new WeakMap();
function snapshot(form) {
  if(!form)return '';
  return JSON.stringify([...form.elements].filter(input=>input.name&&!['submit','button'].includes(input.type)).map(input=>[input.name,input.type==='checkbox'?input.checked:input.type==='file'?[...input.files].map(file=>[file.name,file.size,file.lastModified]):input.multiple?[...input.selectedOptions].map(option=>option.value):input.value]));
}
export function modalIsDirty(dialog) {
  const session=sessions.get(dialog);
  return Boolean(session?.ready&&snapshot(dialog.querySelector('form'))!==session.initial);
}
export function ensureDialog(root) {
  let dialog=root.querySelector('#app-modal');
  if(dialog)return dialog;
  dialog=document.createElement('dialog');dialog.id='app-modal';dialog.setAttribute('aria-labelledby','dialog-title');root.append(dialog);
  const keepEditing=()=>{
    dialog.querySelector('[data-discard-prompt]')?.remove();
    for(const target of dialog.querySelectorAll('form,.dialog-head,.dialog-actions'))target.inert=false;
    dialog.querySelector('form input:not([type=hidden]),form textarea,form select')?.focus();
  };
  const requestClose=()=>{
    if(dialog.querySelector('form')?.dataset.saving==='true')return;
    if(!modalIsDirty(dialog)){dialog.close();return;}
    if(dialog.querySelector('[data-discard-prompt]'))return;
    for(const target of dialog.querySelectorAll('form,.dialog-head,.dialog-actions'))target.inert=true;
    const warning=document.createElement('section');warning.dataset.discardPrompt='';warning.className='discard-prompt';
    warning.setAttribute('role','alertdialog');warning.setAttribute('aria-labelledby','discard-title');
    warning.innerHTML='<h3 id="discard-title">Discard unsaved changes?</h3><p>Your edits have not been saved.</p><div class="toolbar"><button type="button" class="pixel-button gold" data-keep-editing>Keep editing</button><button type="button" class="pixel-button danger" data-discard-changes>Discard changes</button></div>';
    dialog.querySelector('.dialog-body').append(warning);warning.querySelector('button').focus();warning.scrollIntoView({block:'nearest'});
  };
  dialog.addEventListener('click',event=>{
    if(event.target.closest('[data-keep-editing]')){keepEditing();return;}
    if(event.target.closest('[data-discard-changes]')){dialog.close();return;}
    if(event.target===dialog||event.target.closest('[data-close]'))requestClose();
  });
  dialog.addEventListener('cancel',event=>{event.preventDefault();if(dialog.querySelector('[data-discard-prompt]'))keepEditing();else requestClose();});
  dialog.addEventListener('close',()=>{
    const session=sessions.get(dialog),previous=session?.focus;
    if(previous?.isConnected)previous.focus();
    else if(session?.action)root.querySelector(`[data-action="${CSS.escape(session.action)}"]${session.id?`[data-id="${CSS.escape(session.id)}"]`:''}`)?.focus();
    sessions.delete(dialog);
  });
  return dialog;
}
export function openModal(dialog,title,body,submitLabel='SAVE') {
  const previous=document.activeElement,existing=sessions.get(dialog);
  sessions.set(dialog,{focus:existing?.focus||previous,action:existing?.action||previous?.dataset?.action,id:existing?.id||previous?.dataset?.id,ready:false});
  dialog.innerHTML=`<div class="dialog-head"><h2 class="pixel-title" id="dialog-title">${esc(title)}</h2><button class="dialog-close" type="button" data-close aria-label="Close dialog">×</button></div><div class="dialog-body">${body}<p class="form-error" role="alert" hidden></p></div><div class="dialog-actions"><button class="pixel-button soft" type="button" data-close>CANCEL</button><button class="pixel-button gold" type="submit" form="modal-form">${esc(submitLabel)}</button></div>`;
  if(!dialog.open)dialog.showModal();
  queueMicrotask(()=>{
    const session=sessions.get(dialog);if(!session)return;
    session.initial=snapshot(dialog.querySelector('form'));session.ready=true;
    (dialog.querySelector('input:not([type=hidden]):not([disabled]),select:not([disabled]),textarea:not([disabled])')||dialog.querySelector('button'))?.focus();
  });
}
// Successful saves and explicit completed actions bypass the discard prompt.
export function closeModal(dialog) {if(dialog.open)dialog.close();}

window.addEventListener('beforeunload',event=>{
  const dialog=document.querySelector('#app-modal');
  if(dialog?.open&&modalIsDirty(dialog)){event.preventDefault();event.returnValue='';}
});
