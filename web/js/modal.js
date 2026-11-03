export function ensureDialog(root) {
  let dialog = root.querySelector('#app-modal');
  if (dialog) return dialog;
  dialog = document.createElement('dialog');
  dialog.id = 'app-modal';
  dialog.setAttribute('aria-labelledby', 'dialog-title');
  root.append(dialog);
  dialog.addEventListener('click', (event) => {
    if (event.target === dialog) dialog.close();
    if (event.target.closest('[data-close]')) dialog.close();
  });
  return dialog;
}

export function openModal(dialog, title, body, submitLabel = 'SAVE') {
  dialog.innerHTML = `
    <div class="dialog-head"><h2 class="pixel-title" id="dialog-title">${title}</h2><button class="dialog-close" type="button" data-close aria-label="Close dialog">×</button></div>
    <div class="dialog-body">${body}</div>
    <div class="dialog-actions"><button class="pixel-button soft" type="button" data-close>CANCEL</button><button class="pixel-button gold" type="submit" form="modal-form">${submitLabel}</button></div>`;
  if (!dialog.open) dialog.showModal();
}

export function closeModal(dialog) { if (dialog.open) dialog.close(); }
