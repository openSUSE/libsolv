@ECHO OFF
SETLOCAL EnableDelayedExpansion
SET cmd=%1
SET dir=%2

SET ex=0

FOR /f "tokens=*" %%G IN ('dir /b %dir%\*.t') DO (
  ECHO "%dir%\%%G"
  CALL %cmd% "%dir%\%%G"

  IF !ERRORLEVEL! EQU 0 (
    ECHO "PASSED";
  ) ELSE (
    IF !ERRORLEVEL! EQU 77 (
      ECHO "SKIPPED";
    ) ELSE (
      ECHO "FAILED";
      SET ex=1;
    )
  )
)

exit /B %ex%