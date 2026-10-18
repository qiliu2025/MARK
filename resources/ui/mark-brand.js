/* MARK shell presentation. All data and download actions remain in ui.js. */
(() => {
  'use strict';
  const byId = id => document.getElementById(id);
  const shell = document.querySelector('.app-shell');
  const release = window.MARK_RELEASE_INFO || {};
  const values = {markAppVersion:release.version, markBuildDate:release.buildDate,
    markKnowledgeThrough:release.dataThrough};
  Object.entries(values).forEach(([id,value]) => { if (value && byId(id)) byId(id).textContent = value; });
  byId('markRepositoryLink')?.addEventListener('click', event => {
    if (!window.chrome?.webview) return;
    event.preventDefault();
    window.chrome.webview.postMessage(JSON.stringify({type:'open-project-repository'}));
  });
  let activeLayer = null;
  let returnFocus = null;
  const triggerFor = new Map([
    [byId('markAboutLayer'), byId('markAboutTrigger')]
  ]);

  function closeLayer(restoreFocus = true) {
    if (!activeLayer) return;
    const target = returnFocus;
    activeLayer.classList.add('hidden');
    activeLayer.setAttribute('aria-hidden', 'true');
    triggerFor.get(activeLayer)?.setAttribute('aria-expanded', 'false');
    activeLayer = null;
    returnFocus = null;
    shell.inert = false;
    if (restoreFocus && target?.isConnected) target.focus({ preventScroll: true });
  }

  function openLayer(layer, trigger) {
    if (activeLayer === layer) { closeLayer(); return; }
    closeLayer(false);
    returnFocus = trigger;
    activeLayer = layer;
    shell.inert = true;
    layer.classList.remove('hidden');
    layer.setAttribute('aria-hidden', 'false');
    trigger.setAttribute('aria-expanded', 'true');
    layer.querySelector('[role="dialog"]').focus({ preventScroll: true });
  }

  triggerFor.forEach((trigger, layer) => {
    trigger.addEventListener('click', () => openLayer(layer, trigger));
    layer.querySelectorAll('[data-mark-close]').forEach(button => {
      button.addEventListener('click', () => closeLayer());
    });
  });

  document.addEventListener('keydown', event => {
    if (!activeLayer) return;
    if (event.key === 'Escape') { event.preventDefault(); closeLayer(); return; }
    if (event.key !== 'Tab') return;
    const dialog = activeLayer.querySelector('[role="dialog"]');
    const items = [...dialog.querySelectorAll('button, input, select, a[href], [tabindex="0"]')]
      .filter(node => !node.disabled && node.getClientRects().length);
    if (!items.length) { event.preventDefault(); dialog.focus(); return; }
    const first = items[0], last = items[items.length - 1];
    const focused = document.activeElement;
    if (event.shiftKey && (focused === first || focused === dialog)) {
      event.preventDefault(); last.focus();
    } else if (!event.shiftKey && (focused === last || focused === dialog)) {
      event.preventDefault(); first.focus();
    }
  });

})();
