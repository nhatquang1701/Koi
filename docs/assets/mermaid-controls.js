(function () {
  function makeButton(label, title, action) {
    var button = document.createElement('button');
    button.type = 'button';
    button.textContent = label;
    button.title = title;
    button.addEventListener('click', action);
    return button;
  }

  function wrap(block) {
    if (block.closest('.mermaid-viewport')) { return; }
    var svg = block.querySelector('svg');
    if (!svg) { return; }
    var viewport = document.createElement('div');
    viewport.className = 'mermaid-viewport';
    block.parentNode.insertBefore(viewport, block);
    viewport.appendChild(block);

    var box = svg.viewBox && svg.viewBox.baseVal;
    var baseWidth = (box && box.width) || svg.getBoundingClientRect().width || 800;
    var baseHeight = (box && box.height) || svg.getBoundingClientRect().height || 600;
    var zoom = 1;

    function apply() {
      svg.style.width = (baseWidth * zoom) + 'px';
      svg.style.height = (baseHeight * zoom) + 'px';
    }

    var bar = document.createElement('div');
    bar.className = 'mermaid-controls';
    bar.appendChild(makeButton('\u2190', 'Scroll left', function () {
      viewport.scrollLeft -= Math.max(240, viewport.clientWidth * 0.6);
    }));
    bar.appendChild(makeButton('\u2192', 'Scroll right', function () {
      viewport.scrollLeft += Math.max(240, viewport.clientWidth * 0.6);
    }));
    bar.appendChild(makeButton('Zoom +', 'Zoom in', function () {
      zoom = Math.min(4, zoom * 1.25);
      apply();
    }));
    bar.appendChild(makeButton('Zoom \u2212', 'Zoom out', function () {
      zoom = Math.max(0.2, zoom * 0.8);
      apply();
    }));
    bar.appendChild(makeButton('Fit', 'Fit to width', function () {
      zoom = Math.max(0.2, (viewport.clientWidth - 24) / baseWidth);
      apply();
      viewport.scrollLeft = 0;
      viewport.scrollTop = 0;
    }));
    bar.appendChild(makeButton('Reset', 'Reset zoom and scroll', function () {
      zoom = 1;
      apply();
      viewport.scrollLeft = 0;
      viewport.scrollTop = 0;
    }));

    viewport.parentNode.insertBefore(bar, viewport);
    apply();
  }

  function enhance() {
    document.querySelectorAll('pre.mermaid, div.mermaid').forEach(function (block) {
      if (block.querySelector('svg')) { wrap(block); }
    });
  }

  function start() {
    enhance();
    var attempts = 0;
    var timer = setInterval(function () {
      enhance();
      if (++attempts > 60) { clearInterval(timer); }
    }, 200);
  }

  if (typeof document$ !== 'undefined' && document$.subscribe) {
    document$.subscribe(start);
  } else if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', start);
  } else {
    start();
  }
})();
