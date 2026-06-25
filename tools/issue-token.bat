@echo off
REM =============================================================================
REM aima game starter - TOKEN ISSUER (Windows).
REM
REM Double-click this. It runs the shared, cross-platform core (issue_token.py),
REM which:
REM   1. Generates a unique bind token  <slug>-<8 hex>  (slug = this folder's name,
REM      lowercased). IDEMPOTENT: reuses this project's existing token if one is
REM      already in the shared registry (never reissues).
REM   2. Registers/updates this project in the SHARED registry the live-dev
REM      Telegram brain reads:  %USERPROFILE%\.aima\registry.json  (created if
REM      missing). Key = project name; value = repo / desc / build / run / shot /
REM      bind_token.
REM   3. Prints the token + how to bind it in the Telegram group:  /bind <token> .
REM
REM Needs Python 3 on PATH (python.org installer ticks "Add to PATH").
REM =============================================================================
setlocal
echo aima game starter - TOKEN ISSUER
echo ================================

REM Prefer the 'py' launcher (Python.org), fall back to python.exe.
where py >nul 2>&1
if %ERRORLEVEL%==0 (
    py "%~dp0issue_token.py" %*
    goto :done
)
where python >nul 2>&1
if %ERRORLEVEL%==0 (
    python "%~dp0issue_token.py" %*
    goto :done
)
echo Python 3 not found on PATH. Install it from https://www.python.org/downloads/
echo (tick "Add python.exe to PATH"), then run this again.
exit /b 1

:done
echo.
echo (Copy the token above and type  /bind ^<token^>  in your Telegram group.)
pause
