(function () {
  var REFRESH_TTL_MS = 10 * 60 * 1000;
  var TIMESTAMP_KEY = 'koi.source_facts_refreshed_at';
  var refreshPromise = null;

  function formatCount(value) {
    if (typeof value !== 'number' || !isFinite(value)) {
      return String(value);
    }
    if (value >= 1000000) {
      return (value / 1000000).toFixed(1).replace(/\.0$/, '') + 'M';
    }
    if (value >= 1000) {
      return (value / 1000).toFixed(1).replace(/\.0$/, '') + 'k';
    }
    return String(value);
  }

  function applyFactsToDom(facts) {
    if (!facts || typeof facts !== 'object') {
      return;
    }
    document.querySelectorAll('.md-source__facts').forEach(function (list) {
      if (facts.version) {
        var ver = list.querySelector('.md-source__fact--version');
        if (ver) {
          ver.textContent = facts.version;
        }
      }
      if (typeof facts.stars === 'number') {
        var stars = list.querySelector('.md-source__fact--stars');
        if (stars) {
          stars.textContent = formatCount(facts.stars);
        }
      }
      if (typeof facts.forks === 'number') {
        var forks = list.querySelector('.md-source__fact--forks');
        if (forks) {
          forks.textContent = formatCount(facts.forks);
        }
      }
    });
  }

  function extractRepoSlug() {
    var link = document.querySelector('a.md-source[href*="github.com/"]');
    if (!link || !link.href) {
      return null;
    }
    var match = link.href.match(/github\.com\/([^/]+)\/([^/?#]+)/i);
    if (!match) {
      return null;
    }
    return match[1] + '/' + match[2].replace(/\.git$/i, '');
  }

  function fetchLatestFacts(slug) {
    if (refreshPromise) {
      return refreshPromise;
    }
    var base = 'https://api.github.com/repos/' + slug;
    var releaseReq = fetch(base + '/releases/latest', { cache: 'no-cache' })
      .then(function (resp) { return resp.ok ? resp.json() : null; })
      .catch(function () { return null; });
    var repoReq = fetch(base, { cache: 'no-cache' })
      .then(function (resp) { return resp.ok ? resp.json() : null; })
      .catch(function () { return null; });

    refreshPromise = Promise.all([releaseReq, repoReq])
      .then(function (results) {
        var release = results[0];
        var repo = results[1];
        if (!release && !repo) {
          return null;
        }
        var current = {};
        try {
          if (typeof __md_get === 'function') {
            current = __md_get('__source', sessionStorage) || {};
          }
        } catch (err) {
          current = {};
        }
        var updated = Object.assign({}, current);
        if (release && typeof release.tag_name === 'string' && release.tag_name) {
          updated.version = release.tag_name;
        }
        if (repo && typeof repo.stargazers_count === 'number') {
          updated.stars = repo.stargazers_count;
        }
        if (repo && typeof repo.forks_count === 'number') {
          updated.forks = repo.forks_count;
        }
        try {
          if (typeof __md_set === 'function') {
            __md_set('__source', updated, sessionStorage);
          }
          sessionStorage.setItem(TIMESTAMP_KEY, String(Date.now()));
        } catch (err) {
          // Ignore storage quota / private-browsing failures.
        }
        applyFactsToDom(updated);
        setTimeout(function () { applyFactsToDom(updated); }, 300);
        return updated;
      })
      .finally(function () {
        refreshPromise = null;
      });
    return refreshPromise;
  }

  function refreshIfStale() {
    var cached = null;
    var lastRefreshed = 0;
    try {
      if (typeof __md_get === 'function') {
        cached = __md_get('__source', sessionStorage);
      }
      lastRefreshed = Number(sessionStorage.getItem(TIMESTAMP_KEY) || '0');
    } catch (err) {
      return;
    }

    if (!cached) {
      // Fresh tab: Material itself fetches live GitHub API data now.
      try {
        sessionStorage.setItem(TIMESTAMP_KEY, String(Date.now()));
      } catch (err) {
        // Ignore storage restrictions.
      }
      return;
    }

    if (lastRefreshed > 0 && Date.now() - lastRefreshed < REFRESH_TTL_MS) {
      return;
    }

    var slug = extractRepoSlug();
    if (!slug || typeof fetch !== 'function') {
      return;
    }
    fetchLatestFacts(slug);
  }

  if (typeof document$ !== 'undefined' && document$.subscribe) {
    document$.subscribe(refreshIfStale);
  } else if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', refreshIfStale);
  } else {
    refreshIfStale();
  }
})();
