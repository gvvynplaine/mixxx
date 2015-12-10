/***************************************************************************
                          controllerengine.cpp  -  description
                          -------------------
    begin                : Sat Apr 30 2011
    copyright            : (C) 2011 by Sean M. Pappalardo
    email                : spappalardo@mixxx.org
 ***************************************************************************/

#include "controllers/controllerengine.h"

#include "controllers/controller.h"
#include "controlobject.h"
#include "controlobjectthread.h"
#include "errordialoghandler.h"
#include "playermanager.h"
// to tell the msvs compiler about `isnan`
#include "util/math.h"
#include "util/time.h"

// Used for id's inside controlConnection objects
// (closure compatible version of connectControl)
#include <QUuid>

const int kDecks = 16;

// Use 1ms for the Alpha-Beta dt. We're assuming the OS actually gives us a 1ms
// timer.
const int kScratchTimerMs = 1;
const double kAlphaBetaDt = kScratchTimerMs / 1000.0;

ControllerEngine::ControllerEngine(Controller* controller)
        : m_pEngine(NULL),
          m_pController(controller),
          m_bDebug(false),
          m_bPopups(false),
          m_pBaClass(NULL) {
    // Handle error dialog buttons
    qRegisterMetaType<QMessageBox::StandardButton>("QMessageBox::StandardButton");

    // Pre-allocate arrays for average number of virtual decks
    m_intervalAccumulator.resize(kDecks);
    m_lastMovement.resize(kDecks);
    m_dx.resize(kDecks);
    m_rampTo.resize(kDecks);
    m_ramp.resize(kDecks);
    m_scratchFilters.resize(kDecks);
    m_rampFactor.resize(kDecks);
    m_brakeActive.resize(kDecks);
    // Initialize arrays used for testing and pointers
    for (int i = 0; i < kDecks; ++i) {
        m_dx[i] = 0.0;
        m_scratchFilters[i] = new AlphaBetaFilter();
        m_ramp[i] = false;
    }

    initializeScriptEngine();
}

ControllerEngine::~ControllerEngine() {
    // Clean up
    for (int i = 0; i < kDecks; ++i) {
        delete m_scratchFilters[i];
        m_scratchFilters[i] = NULL;
    }

    // Delete the script engine, first clearing the pointer so that
    // other threads will not get the dead pointer after we delete it.
    if (m_pEngine != NULL) {
        QScriptEngine *engine = m_pEngine;
        m_pEngine = NULL;
        engine->deleteLater();
    }
}

/* -------- ------------------------------------------------------
Purpose: Calls the same method on a list of JS Objects
Input:   -
Output:  -
-------- ------------------------------------------------------ */
void ControllerEngine::callFunctionOnObjects(QList<QString> scriptFunctionPrefixes,
                                             QString function, QScriptValueList args) {
    const QScriptValue global = m_pEngine->globalObject();

    foreach (QString prefixName, scriptFunctionPrefixes) {
        QScriptValue prefix = global.property(prefixName);
        if (!prefix.isValid() || !prefix.isObject()) {
            qWarning() << "ControllerEngine: No" << prefixName << "object in script";
            continue;
        }

        QScriptValue init = prefix.property(function);
        if (!init.isValid() || !init.isFunction()) {
            qWarning() << "ControllerEngine:" << prefixName << "has no" << function << " method";
            continue;
        }
        if (m_bDebug) {
            qDebug() << "ControllerEngine: Executing" << prefixName << "." << function;
        }
        init.call(prefix, args);
    }
}

/* -------- ------------------------------------------------------
Purpose: Resolves a function name to a QScriptValue including
            OBJECT.Function calls
Input:   -
Output:  -
-------- ------------------------------------------------------ */
QScriptValue ControllerEngine::resolveFunction(QString function, bool useCache) const {
    if (useCache && m_scriptValueCache.contains(function)) {
        return m_scriptValueCache.value(function);
    }

    QScriptValue object = m_pEngine->globalObject();
    QStringList parts = function.split(".");

    for (int i = 0; i < parts.size(); i++) {
        object = object.property(parts.at(i));
        if (!object.isValid())
            return QScriptValue();
    }

    if (!object.isFunction()) {
        return QScriptValue();
    }
    m_scriptValueCache[function] = object;
    return object;
}

/* -------- ------------------------------------------------------
Purpose: Shuts down scripts in an orderly fashion
            (stops timers then executes shutdown functions)
Input:   -
Output:  -
-------- ------------------------------------------------------ */
void ControllerEngine::gracefulShutdown() {
    qDebug() << "ControllerEngine shutting down...";

    // Clear the m_connectedControls hash so we stop responding
    // to signals.
    m_connectedControls.clear();

    // Stop all timers
    stopAllTimers();

    // Call each script's shutdown function if it exists
    callFunctionOnObjects(m_scriptFunctionPrefixes, "shutdown");

    // Prevents leaving decks in an unstable state
    //  if the controller is shut down while scratching
    QHashIterator<int, int> i(m_scratchTimers);
    while (i.hasNext()) {
        i.next();
        qDebug() << "Aborting scratching on deck" << i.value();
        // Clear scratch2_enable. PlayerManager::groupForDeck is 0-indexed.
        QString group = PlayerManager::groupForDeck(i.value() - 1);
        ControlObjectThread* pScratch2Enable =
                getControlObjectThread(group, "scratch2_enable");
        if (pScratch2Enable != NULL) {
            pScratch2Enable->slotSet(0);
        }
    }

    // Clear the Script Value cache
    m_scriptValueCache.clear();

    // Free all the control object threads
    QList<ConfigKey> keys = m_controlCache.keys();
    QList<ConfigKey>::iterator it = keys.begin();
    QList<ConfigKey>::iterator end = keys.end();
    while (it != end) {
        ConfigKey key = *it;
        ControlObjectThread* cot = m_controlCache.take(key);
        delete cot;
        ++it;
    }

    delete m_pBaClass;
    m_pBaClass = NULL;
}

bool ControllerEngine::isReady() {
    bool ret = m_pEngine != NULL;
    return ret;
}

void ControllerEngine::initializeScriptEngine() {
    // Create the Script Engine
    m_pEngine = new QScriptEngine(this);

    // Make this ControllerEngine instance available to scripts as 'engine'.
    QScriptValue engineGlobalObject = m_pEngine->globalObject();
    engineGlobalObject.setProperty("engine", m_pEngine->newQObject(this));

    if (m_pController) {
        qDebug() << "Controller in script engine is:" << m_pController->getName();

        // Make the Controller instance available to scripts
        engineGlobalObject.setProperty("controller", m_pEngine->newQObject(m_pController));

        // ...under the legacy name as well
        engineGlobalObject.setProperty("midi", m_pEngine->newQObject(m_pController));
    }

    m_pBaClass = new ByteArrayClass(m_pEngine);
    engineGlobalObject.setProperty("ByteArray", m_pBaClass->constructor());
}

/* -------- ------------------------------------------------------
   Purpose: Load all script files given in the supplied list
   Input:   Global ConfigObject, QString list of file names to load
   Output:  -
   -------- ------------------------------------------------------ */
void ControllerEngine::loadScriptFiles(QList<QString> scriptPaths,
                                       const QList<ControllerPreset::ScriptFileInfo>& scripts) {
    // Set the Debug flag
    if (m_pController)
        m_bDebug = m_pController->debugging();

    qDebug() << "ControllerEngine: Loading & evaluating all script code";

    m_lastScriptPaths = scriptPaths;

    // scriptPaths holds the paths to search in when we're looking for scripts
    foreach (const ControllerPreset::ScriptFileInfo& script, scripts) {
        evaluate(script.name, scriptPaths);

        if (m_scriptErrors.contains(script.name)) {
            qDebug() << "Errors occured while loading " << script.name;
        }
    }

    connect(&m_scriptWatcher, SIGNAL(fileChanged(QString)),
            this, SLOT(scriptHasChanged(QString)));

    emit(initialized());
}

// Slot to run when a script file has changed
void ControllerEngine::scriptHasChanged(QString scriptFilename) {
    Q_UNUSED(scriptFilename);
    qDebug() << "ControllerEngine: Reloading Scripts";
    ControllerPresetPointer pPreset = m_pController->getPreset();

    disconnect(&m_scriptWatcher, SIGNAL(fileChanged(QString)),
               this, SLOT(scriptHasChanged(QString)));

    gracefulShutdown();

    // Delete the script engine, first clearing the pointer so that
    // other threads will not get the dead pointer after we delete it.
    if (m_pEngine != NULL) {
        QScriptEngine *engine = m_pEngine;
        m_pEngine = NULL;
        engine->deleteLater();
    }

    initializeScriptEngine();
    loadScriptFiles(m_lastScriptPaths, pPreset->scripts);

    qDebug() << "Re-initializing scripts";
    initializeScripts(pPreset->scripts);
}

/* -------- ------------------------------------------------------
   Purpose: Run the initialization function for each loaded script
                if it exists
   Input:   -
   Output:  -
   -------- ------------------------------------------------------ */
void ControllerEngine::initializeScripts(const QList<ControllerPreset::ScriptFileInfo>& scripts) {

    m_scriptFunctionPrefixes.clear();
    foreach (const ControllerPreset::ScriptFileInfo& script, scripts) {
        m_scriptFunctionPrefixes.append(script.functionPrefix);
    }

    QScriptValueList args;
    args << QScriptValue(m_pController->getName());
    args << QScriptValue(m_bDebug);

    // Call the init method for all the prefixes.
    callFunctionOnObjects(m_scriptFunctionPrefixes, "init", args);

    emit(initialized());
}

/* -------- ------------------------------------------------------
   Purpose: Validate script syntax, then evaluate() it so the
            functions are registered & available for use.
   Input:   -
   Output:  -
   -------- ------------------------------------------------------ */
bool ControllerEngine::evaluate(QString filepath) {
    QList<QString> dummy;
    bool ret = evaluate(filepath, dummy);

    return ret;
}

/* -------- ------------------------------------------------------
   Purpose: Evaluate & call a script function
   Input:   Function name
   Output:  false if an invalid function or an exception
   -------- ------------------------------------------------------ */
bool ControllerEngine::execute(QString function) {
    if (m_pEngine == NULL)
        return false;

    QScriptValue scriptFunction = m_pEngine->evaluate(function);

    if (checkException())
        return false;

    if (!scriptFunction.isFunction())
        return false;

    scriptFunction.call(QScriptValue());
    if (checkException())
        return false;

    return true;
}


/* -------- ------------------------------------------------------
Purpose: Evaluate & run script code
Input:   'this' object if applicable, Code string
Output:  false if an exception
-------- ------------------------------------------------------ */
bool ControllerEngine::internalExecute(QScriptValue thisObject,
                                       QString scriptCode) {
    // A special version of safeExecute since we're evaluating strings, not actual functions
    //  (execute() would print an error that it's not a function every time a timer fires.)
    if (m_pEngine == NULL)
        return false;

    // Check syntax
    QScriptSyntaxCheckResult result = m_pEngine->checkSyntax(scriptCode);
    QString error = "";
    switch (result.state()) {
        case (QScriptSyntaxCheckResult::Valid): break;
        case (QScriptSyntaxCheckResult::Intermediate):
            error = "Incomplete code";
            break;
        case (QScriptSyntaxCheckResult::Error):
            error = "Syntax error";
            break;
    }
    if (error!="") {
        error = QString("%1: %2 at line %3, column %4 of script code:\n%5\n")
                .arg(error,
                     result.errorMessage(),
                     QString::number(result.errorLineNumber()),
                     QString::number(result.errorColumnNumber()),
                     scriptCode);

        scriptErrorDialog(error);
        return false;
    }

    QScriptValue scriptFunction = m_pEngine->evaluate(scriptCode);

    if (checkException()) {
        qDebug() << "Exception";
        return false;
    }

    return internalExecute(thisObject, scriptFunction);
}

/* -------- ------------------------------------------------------
Purpose: Evaluate & run script code
Input:   'this' object if applicable, Code string
Output:  false if an exception
-------- ------------------------------------------------------ */
bool ControllerEngine::internalExecute(QScriptValue thisObject, QScriptValue functionObject) {
    if(m_pEngine == NULL)
        return false;

    // If it's not a function, we're done.
    if (!functionObject.isFunction()) {
        return false;
    }

    // If it does happen to be a function, call it.
    functionObject.call(thisObject);
    if (checkException()) {
        qDebug() << "Exception";
        return false;
    }

    return true;
}

/**-------- ------------------------------------------------------
   Purpose: Evaluate & call a script function with argument list
   Input:   Function name, argument list
   Output:  false if an invalid function or an exception
   -------- ------------------------------------------------------ */
bool ControllerEngine::execute(QString function, QScriptValueList args) {
    if(m_pEngine == NULL) {
        qDebug() << "ControllerEngine::execute: No script engine exists!";
        return false;
    }

    QScriptValue scriptFunction = m_pEngine->evaluate(function);

    if (checkException())
        return false;

    return execute(scriptFunction, args);
}

/**-------- ------------------------------------------------------
   Purpose: Evaluate & call a script function with argument list
   Input:   Function name, argument list
   Output:  false if an invalid function or an exception
   -------- ------------------------------------------------------ */
bool ControllerEngine::execute(QScriptValue functionObject, QScriptValueList args) {

    if(m_pEngine == NULL) {
        qDebug() << "ControllerEngine::execute: No script engine exists!";
        return false;
    }

    if (!functionObject.isFunction()) {
        qDebug() << "Not a function";
        return false;
    }
    QScriptValue rc = functionObject.call(m_pEngine->globalObject(), args);
    if (!rc.isValid()) {
        qDebug() << "QScriptValue is not a function or ...";
        return false;
    }

    if (checkException())
        return false;
    return true;
}
/**-------- ------------------------------------------------------
   Purpose: Evaluate & call a script function
   Input:   Function name, data string (e.g. device ID)
   Output:  false if an invalid function or an exception
   -------- ------------------------------------------------------ */
bool ControllerEngine::execute(QString function, QString data) {
    if (m_pEngine == NULL) {
        qDebug() << "ControllerEngine::execute: No script engine exists!";
        return false;
    }

    QScriptValue scriptFunction = m_pEngine->evaluate(function);

    if (checkException()) {
        qDebug() << "ControllerEngine::execute: Exception";
        return false;
    }

    if (!scriptFunction.isFunction()) {
        qDebug() << "ControllerEngine::execute: Not a function";
        return false;
    }

    QScriptValueList args;
    args << QScriptValue(data);

    return execute(scriptFunction, args);
}

/**-------- ------------------------------------------------------
   Purpose: Evaluate & call a script function
   Input:   Function name, ponter to data buffer, length of buffer
   Output:  false if an invalid function or an exception
   -------- ------------------------------------------------------ */
bool ControllerEngine::execute(QString function, const QByteArray data) {
    if (m_pEngine == NULL) {
        return false;
    }

    if (!m_pEngine->canEvaluate(function)) {
        qWarning() << "ControllerEngine: ?Syntax error in function" << function;
        return false;
    }

    QScriptValue scriptFunction = m_pEngine->evaluate(function);

    if (checkException())
        return false;

    return execute(scriptFunction, data);
}

/**-------- ------------------------------------------------------
   Purpose: Evaluate & call a script function
   Input:   Function name, ponter to data buffer, length of buffer
   Output:  false if an invalid function or an exception
   -------- ------------------------------------------------------ */
bool ControllerEngine::execute(QScriptValue function, const QByteArray data) {
    if (m_pEngine == NULL) {
        return false;
    }

    if (checkException())
        return false;
    if (!function.isFunction())
        return false;

    QScriptValueList args;
    args << QScriptValue(m_pBaClass->newInstance(data));
    args << QScriptValue(data.size());

    return execute(function, args);
}

/* -------- ------------------------------------------------------
   Purpose: Check to see if a script threw an exception
   Input:   QScriptValue returned from call(scriptFunctionName)
   Output:  true if there was an exception
   -------- ------------------------------------------------------ */
bool ControllerEngine::checkException() {
    if(m_pEngine == NULL) {
        return false;
    }

    if (m_pEngine->hasUncaughtException()) {
        QScriptValue exception = m_pEngine->uncaughtException();
        QString errorMessage = exception.toString();
        int line = m_pEngine->uncaughtExceptionLineNumber();
        QStringList backtrace = m_pEngine->uncaughtExceptionBacktrace();
        QString filename = exception.property("fileName").toString();

        QStringList error;
        error << (filename.isEmpty() ? "" : filename) << errorMessage << QString(line);
        m_scriptErrors.insert((filename.isEmpty() ? "passed code" : filename), error);

        QString errorText = tr("Uncaught exception at line %1 in file %2: %3")
                .arg(QString::number(line),
                     (filename.isEmpty() ? "" : filename),
                     errorMessage);

        if (filename.isEmpty())
            errorText = tr("Uncaught exception at line %1 in passed code: %2")
                    .arg(QString::number(line), errorMessage);

        scriptErrorDialog(m_bDebug ? QString("%1\nBacktrace:\n%2")
                          .arg(errorText, backtrace.join("\n")) : errorText);
        return true;
    }
    return false;
}

/*  -------- ------------------------------------------------------
    Purpose: Common error dialog creation code for run-time exceptions
                Allows users to ignore the error or reload the mappings
    Input:   Detailed error string
    Output:  -
    -------- ------------------------------------------------------ */
void ControllerEngine::scriptErrorDialog(QString detailedError) {
    qWarning() << "ControllerEngine:" << detailedError;
    ErrorDialogProperties* props = ErrorDialogHandler::instance()->newDialogProperties();
    props->setType(DLG_WARNING);
    props->setTitle(tr("Controller script error"));
    props->setText(tr("A control you just used is not working properly."));
    props->setInfoText("<html>"+tr("The script code needs to be fixed.")+
        "<p>"+tr("For now, you can: Ignore this error for this session but you may experience erratic behavior.")+
        "<br>"+tr("Try to recover by resetting your controller.")+"</p>"+"</html>");
    props->setDetails(detailedError);
    props->setKey(detailedError);   // To prevent multiple windows for the same error

    // Allow user to suppress further notifications about this particular error
    props->addButton(QMessageBox::Ignore);

    props->addButton(QMessageBox::Retry);
    props->addButton(QMessageBox::Close);
    props->setDefaultButton(QMessageBox::Close);
    props->setEscapeButton(QMessageBox::Close);
    props->setModal(false);

    if (ErrorDialogHandler::instance()->requestErrorDialog(props)) {
        // Enable custom handling of the dialog buttons
        connect(ErrorDialogHandler::instance(), SIGNAL(stdButtonClicked(QString, QMessageBox::StandardButton)),
                this, SLOT(errorDialogButton(QString, QMessageBox::StandardButton)));
    }
}

/* -------- ------------------------------------------------------
    Purpose: Slot to handle custom button clicks in error dialogs
    Input:   Key of dialog, StandardButton that was clicked
    Output:  -
    -------- ------------------------------------------------------ */
void ControllerEngine::errorDialogButton(QString key, QMessageBox::StandardButton button) {
    Q_UNUSED(key);

    // Something was clicked, so disable this signal now
    disconnect(ErrorDialogHandler::instance(),
               SIGNAL(stdButtonClicked(QString, QMessageBox::StandardButton)),
               this,
               SLOT(errorDialogButton(QString, QMessageBox::StandardButton)));

    if (button == QMessageBox::Retry) {
        emit(resetController());
    }
}

ControlObjectThread* ControllerEngine::getControlObjectThread(QString group, QString name) {
    ConfigKey key = ConfigKey(group, name);
    ControlObjectThread* cot = m_controlCache.value(key, NULL);
    if (cot == NULL) {
        // create COT
        cot = new ControlObjectThread(key, this);
        if (cot->valid()) {
            m_controlCache.insert(key, cot);
        } else {
            delete cot;
            cot = NULL;
        }
    }
    return cot;
}

/* -------- ------------------------------------------------------
   Purpose: Returns the current value of a Mixxx control (for scripts)
   Input:   Control group (e.g. [Channel1]), Key name (e.g. [filterHigh])
   Output:  The value
   -------- ------------------------------------------------------ */
double ControllerEngine::getValue(QString group, QString name) {
    ControlObjectThread* cot = getControlObjectThread(group, name);
    if (cot == NULL) {
        qWarning() << "ControllerEngine: Unknown control" << group << name << ", returning 0.0";
        return 0.0;
    }
    return cot->get();
}

/* -------- ------------------------------------------------------
   Purpose: Sets new value of a Mixxx control (for scripts)
   Input:   Control group, Key name, new value
   Output:  -
   -------- ------------------------------------------------------ */
void ControllerEngine::setValue(QString group, QString name, double newValue) {
    if (isnan(newValue)) {
        qWarning() << "ControllerEngine: script setting [" << group << "," << name
                 << "] to NotANumber, ignoring.";
        return;
    }

    ControlObjectThread* cot = getControlObjectThread(group, name);

    if (cot != NULL) {
        ControlObject* pControl = ControlObject::getControl(cot->getKey());
        if (pControl && !m_st.ignore(pControl, cot->getParameterForValue(newValue))) {
            cot->slotSet(newValue);
        }
    }
}


/* -------- ------------------------------------------------------
   Purpose: Returns the normalized value of a Mixxx control (for scripts)
   Input:   Control group (e.g. [Channel1]), Key name (e.g. [filterHigh])
   Output:  The value
   -------- ------------------------------------------------------ */
double ControllerEngine::getParameter(QString group, QString name) {
    ControlObjectThread* cot = getControlObjectThread(group, name);
    if (cot == NULL) {
        qWarning() << "ControllerEngine: Unknown control" << group << name << ", returning 0.0";
        return 0.0;
    }
    return cot->getParameter();
}

/* -------- ------------------------------------------------------
   Purpose: Sets new normalized parameter of a Mixxx control (for scripts)
   Input:   Control group, Key name, new value
   Output:  -
   -------- ------------------------------------------------------ */
void ControllerEngine::setParameter(QString group, QString name, double newParameter) {
    if (isnan(newParameter)) {
        qWarning() << "ControllerEngine: script setting [" << group << "," << name
                 << "] to NotANumber, ignoring.";
        return;
    }

    ControlObjectThread* cot = getControlObjectThread(group, name);

    // TODO(XXX): support soft takeover.
    if (cot != NULL) {
        cot->setParameter(newParameter);
    }
}

/* -------- ------------------------------------------------------
   Purpose: normalize a value of a Mixxx control (for scripts)
   Input:   Control group, Key name, new value
   Output:  -
   -------- ------------------------------------------------------ */
double ControllerEngine::getParameterForValue(QString group, QString name, double value) {
    if (isnan(value)) {
        qWarning() << "ControllerEngine: script setting [" << group << "," << name
                 << "] to NotANumber, ignoring.";
        return 0.0;
    }

    ControlObjectThread* cot = getControlObjectThread(group, name);

    if (cot == NULL) {
        qWarning() << "ControllerEngine: Unknown control" << group << name << ", returning 0.0";
        return 0.0;
    }

    return cot->getParameterForValue(value);
}

/* -------- ------------------------------------------------------
   Purpose: Resets the value of a Mixxx control (for scripts)
   Input:   Control group, Key name, new value
   Output:  -
   -------- ------------------------------------------------------ */
void ControllerEngine::reset(QString group, QString name) {
    ControlObjectThread* cot = getControlObjectThread(group, name);
    if (cot != NULL) {
        cot->reset();
    }
}

/* -------- ------------------------------------------------------
   Purpose: default value of a Mixxx control (for scripts)
   Input:   Control group, Key name, new value
   Output:  -
   -------- ------------------------------------------------------ */
double ControllerEngine::getDefaultValue(QString group, QString name) {
    ControlObjectThread* cot = getControlObjectThread(group, name);

    if (cot == NULL) {
        qWarning() << "ControllerEngine: Unknown control" << group << name << ", returning 0.0";
        return 0.0;
    }

    return cot->getDefault();
}

/* -------- ------------------------------------------------------
   Purpose: default parameter of a Mixxx control (for scripts)
   Input:   Control group, Key name, new value
   Output:  -
   -------- ------------------------------------------------------ */
double ControllerEngine::getDefaultParameter(QString group, QString name) {
    ControlObjectThread* cot = getControlObjectThread(group, name);

    if (cot == NULL) {
        qWarning() << "ControllerEngine: Unknown control" << group << name << ", returning 0.0";
        return 0.0;
    }

    return cot->getParameterForValue(cot->getDefault());
}

/* -------- ------------------------------------------------------
   Purpose: qDebugs script output so it ends up in mixxx.log
   Input:   String to log
   Output:  -
   -------- ------------------------------------------------------ */
void ControllerEngine::log(QString message) {
    qDebug() << message;
}

/* -------- ------------------------------------------------------
   Purpose: Emits valueChanged() so device outputs update
   Input:   -
   Output:  -
   -------- ------------------------------------------------------ */
void ControllerEngine::trigger(QString group, QString name) {
    ControlObjectThread* cot = getControlObjectThread(group, name);
    if (cot != NULL) {
        cot->emitValueChanged();
    }
}

/**-------- ------------------------------------------------------
   Purpose: (Dis)connects a ControlObject valueChanged() signal to/from a script function
   Input:   Control group (e.g. [Channel1]), Key name (e.g. [filterHigh]),
                script function name, true if you want to disconnect
   Output:  true if successful
   -------- ------------------------------------------------------ */
QScriptValue ControllerEngine::connectControl(QString group, QString name,
                                              QScriptValue callback, bool disconnect) {
    ConfigKey key(group, name);
    ControlObjectThread* cot = getControlObjectThread(group, name);
    QScriptValue function;

    if (cot == NULL) {
        qWarning() << "ControllerEngine: script connecting [" << group << "," << name
                   << "], which is non-existent. ignoring.";
        return QScriptValue();
    }

    if (m_pEngine == NULL) {
        return QScriptValue(false);
    }

    if (callback.isString()) {
        ControllerEngineConnection cb;
        cb.key = key;
        cb.id = callback.toString();
        cb.ce = this;

        if (disconnect) {
            disconnectControl(cb);
            return QScriptValue(true);
        }

        function = m_pEngine->evaluate(callback.toString());
        if (checkException() || !function.isFunction()) {
            qWarning() << "Could not evaluate callback function:" << callback.toString();
            return QScriptValue(false);
        } else if (m_connectedControls.contains(key, cb)) {
            // Do not allow multiple connections to named functions

            // Return a wrapper to the conn
            QHash<ConfigKey, ControllerEngineConnection>::iterator i =
                m_connectedControls.find(key);

            ControllerEngineConnection conn = i.value();
            return m_pEngine->newQObject(
                new ControllerEngineConnectionScriptValue(conn),
                QScriptEngine::ScriptOwnership);
        }
    } else if (callback.isFunction()) {
        function = callback;
    } else if (callback.isQObject()) {
        // Assume a ControllerEngineConnection
        QObject *qobject = callback.toQObject();
        const QMetaObject *qmeta = qobject->metaObject();

        if (!strcmp(qmeta->className(), "ControllerEngineConnectionScriptValue")) {
            ControllerEngineConnectionScriptValue *proxy =
                (ControllerEngineConnectionScriptValue *)qobject;
            proxy->disconnect();
        }
    } else {
        qWarning() << "Invalid callback";
        return QScriptValue(false);
    }

    if (function.isFunction()) {
        qDebug() << "Connection:" << group << name;
        connect(cot, SIGNAL(valueChanged(double)),
                this, SLOT(slotValueChanged(double)),
                Qt::QueuedConnection);
        connect(cot, SIGNAL(valueChangedByThis(double)),
                this, SLOT(slotValueChanged(double)),
                Qt::QueuedConnection);

        ControllerEngineConnection conn;
        conn.key = key;
        conn.ce = this;
        conn.function = function;

        QScriptContext *ctxt = m_pEngine->currentContext();
        // Our current context is a function call to engine.connectControl. We
        // want to grab the 'this' from the caller's context, so we walk up the
        // stack.
        if (ctxt) {
            ctxt = ctxt->parentContext();
            conn.context = ctxt ? ctxt->thisObject() : QScriptValue();
        }

        if (callback.isString()) {
            conn.id = callback.toString();
        } else {
            QUuid uuid = QUuid::createUuid();
            conn.id = uuid.toString();
        }

        m_connectedControls.insert(key, conn);
        return m_pEngine->newQObject(
            new ControllerEngineConnectionScriptValue(conn),
            QScriptEngine::ScriptOwnership);
    }

    return QScriptValue(false);
}

/* -------- ------------------------------------------------------
   Purpose: (Dis)connects a ControlObject valueChanged() signal to/from a script function
   Input:   Control group (e.g. [Channel1]), Key name (e.g. [filterHigh]),
                script function name, true if you want to disconnect
   Output:  true if successful
   -------- ------------------------------------------------------ */
void ControllerEngine::disconnectControl(const ControllerEngineConnection conn) {
    ControlObjectThread* cot = getControlObjectThread(conn.key.group, conn.key.item);

    if (m_pEngine == NULL) {
        return;
    }

    if (m_connectedControls.contains(conn.key, conn)) {
        m_connectedControls.remove(conn.key, conn);
        // Only disconnect the signal if there are no other instances of this control using it
        if (!m_connectedControls.contains(conn.key)) {
            disconnect(cot, SIGNAL(valueChanged(double)),
                       this, SLOT(slotValueChanged(double)));
            disconnect(cot, SIGNAL(valueChangedByThis(double)),
                       this, SLOT(slotValueChanged(double)));
        }
    } else {
        qWarning() << "Could not Disconnect connection" << conn.id;
    }
}

void ControllerEngineConnectionScriptValue::disconnect() {
    conn.ce->disconnectControl(conn);
}

/**-------- ------------------------------------------------------
   Purpose: Receives valueChanged() slots from ControlObjects, and
   fires off the appropriate script function.
   -------- ------------------------------------------------------ */
void ControllerEngine::slotValueChanged(double value) {
    ControlObjectThread* senderCOT = dynamic_cast<ControlObjectThread*>(sender());
    if (senderCOT == NULL) {
        qWarning() << "ControllerEngine::slotValueChanged() Shouldn't happen -- sender == NULL";
        return;
    }

    ConfigKey key = senderCOT->getKey();

    //qDebug() << "[Controller]: SlotValueChanged" << key.group << key.item;

    if (m_connectedControls.contains(key)) {
        QHash<ConfigKey, ControllerEngineConnection>::iterator iter =
            m_connectedControls.find(key);
        QList<ControllerEngineConnection> conns;

        // Create a temporary list to allow callbacks to disconnect
        // -Phillip Whelan
        while (iter != m_connectedControls.end() && iter.key() == key) {
            conns.append(iter.value());
            ++iter;
        }

        for (int i = 0; i < conns.size(); ++i) {
            ControllerEngineConnection conn = conns.at(i);
            QScriptValueList args;

            args << QScriptValue(value);
            args << QScriptValue(key.group);
            args << QScriptValue(key.item);
            QScriptValue result = conn.function.call(conn.context, args);
            if (result.isError()) {
                qWarning()<< "ControllerEngine: Call to callback" << conn.id
                          << "resulted in an error:" << result.toString();
            }
        }
    } else {
        qWarning() << "ControllerEngine::slotValueChanged() Received signal from ControlObject that is not connected to a script function.";
    }
}

/* -------- ------------------------------------------------------
   Purpose: Evaluate a script file
   Input:   Script filename
   Output:  false if the script file has errors or doesn't exist
   -------- ------------------------------------------------------ */
bool ControllerEngine::evaluate(QString scriptName, QList<QString> scriptPaths) {
    if (m_pEngine == NULL) {
        return false;
    }

    QString filename = "";
    QFile input;

    if (scriptPaths.length() == 0) {
        // If we aren't given any paths to search, assume that scriptName
        // contains the full file name
        filename = scriptName;
        input.setFileName(filename);
    } else {
        foreach (QString scriptPath, scriptPaths) {
            QDir scriptPathDir(scriptPath);
            filename = scriptPathDir.absoluteFilePath(scriptName);
            input.setFileName(filename);
            if (input.exists())  {
                qDebug() << "ControllerEngine: Watching JS File:" << filename;
                m_scriptWatcher.addPath(filename);
                break;
            }
        }
    }

    qDebug() << "ControllerEngine: Loading" << filename;

    // Read in the script file
    if (!input.open(QIODevice::ReadOnly)) {
        QString errorLog =
            QString("ControllerEngine: Problem opening the script file: %1, error # %2, %3")
                .arg(filename, QString("%1").arg(input.error()), input.errorString());

        qWarning() << errorLog;
        if (m_bPopups) {
            // Set up error dialog
            ErrorDialogProperties* props = ErrorDialogHandler::instance()->newDialogProperties();
            props->setType(DLG_WARNING);
            props->setTitle("Controller script file problem");
            props->setText(QString("There was a problem opening the controller script file %1.").arg(filename));
            props->setInfoText(input.errorString());

            // Ask above layer to display the dialog & handle user response
            ErrorDialogHandler::instance()->requestErrorDialog(props);
        }
        return false;
    }

    QString scriptCode = "";
    scriptCode.append(input.readAll());
    scriptCode.append('\n');
    input.close();

    // Check syntax
    QScriptSyntaxCheckResult result = m_pEngine->checkSyntax(scriptCode);
    QString error="";
    switch (result.state()) {
        case (QScriptSyntaxCheckResult::Valid): break;
        case (QScriptSyntaxCheckResult::Intermediate):
            error = "Incomplete code";
            break;
        case (QScriptSyntaxCheckResult::Error):
            error = "Syntax error";
            break;
    }
    if (error != "") {
        error = QString("%1 at line %2, column %3 in file %4: %5")
                    .arg(error,
                         QString::number(result.errorLineNumber()),
                         QString::number(result.errorColumnNumber()),
                         filename, result.errorMessage());

        qWarning() << "ControllerEngine:" << error;
        if (m_bPopups) {
            ErrorDialogProperties* props = ErrorDialogHandler::instance()->newDialogProperties();
            props->setType(DLG_WARNING);
            props->setTitle("Controller script file error");
            props->setText(QString("There was an error in the controller script file %1.").arg(filename));
            props->setInfoText("The functionality provided by this script file will be disabled.");
            props->setDetails(error);

            ErrorDialogHandler::instance()->requestErrorDialog(props);
        }
        return false;
    }

    // Evaluate the code
    QScriptValue scriptFunction = m_pEngine->evaluate(scriptCode, filename);

    // Record errors
    if (checkException()) {
        return false;
    }

    return true;
}

bool ControllerEngine::hasErrors(QString filename) {
    bool ret = m_scriptErrors.contains(filename);
    return ret;
}

const QStringList ControllerEngine::getErrors(QString filename) {
    QStringList ret = m_scriptErrors.value(filename, QStringList());
    return ret;
}


/* -------- ------------------------------------------------------
   Purpose: Creates & starts a timer that runs some script code
                on timeout
   Input:   Number of milliseconds, script function to call,
                whether it should fire just once
   Output:  The timer's ID, 0 if starting it failed
   -------- ------------------------------------------------------ */
int ControllerEngine::beginTimer(int interval, QScriptValue timerCallback,
                                 bool oneShot) {
    if (!timerCallback.isFunction() && !timerCallback.isString()) {
        qWarning() << "Invalid timer callback provided to beginTimer."
                   << "Valid callbacks are strings and functions.";
        return 0;
    }

    if (interval < 20) {
        qWarning() << "Timer request for" << interval
                   << "ms is too short. Setting to the minimum of 20ms.";
        interval = 20;
    }

    // This makes use of every QObject's internal timer mechanism. Nice, clean,
    // and simple. See http://doc.trolltech.com/4.6/qobject.html#startTimer for
    // details
    int timerId = startTimer(interval);
    TimerInfo info;
    info.callback = timerCallback;
    QScriptContext *ctxt = m_pEngine->currentContext();
    info.context = ctxt ? ctxt->thisObject() : QScriptValue();
    info.oneShot = oneShot;
    m_timers[timerId] = info;
    if (timerId == 0) {
        qWarning() << "Script timer could not be created";
    } else if (m_bDebug) {
        if (oneShot)
            qDebug() << "Starting one-shot timer:" << timerId;
        else
            qDebug() << "Starting timer:" << timerId;
    }
    return timerId;
}

/* -------- ------------------------------------------------------
   Purpose: Stops & removes a timer
   Input:   ID of timer to stop
   Output:  -
   -------- ------------------------------------------------------ */
void ControllerEngine::stopTimer(int timerId) {
    if (!m_timers.contains(timerId)) {
        qWarning() << "Killing timer" << timerId << ": That timer does not exist!";
        return;
    }
    if (m_bDebug) {
        qDebug() << "Killing timer:" << timerId;
    }

    killTimer(timerId);
    m_timers.remove(timerId);
}

void ControllerEngine::stopAllTimers() {
    QMutableHashIterator<int, TimerInfo> i(m_timers);
    while (i.hasNext()) {
        i.next();
        stopTimer(i.key());
    }
}

void ControllerEngine::timerEvent(QTimerEvent *event) {
    int timerId = event->timerId();

    // See if this is a scratching timer
    if (m_scratchTimers.contains(timerId)) {
        scratchProcess(timerId);
        return;
    }

    QHash<int, TimerInfo>::const_iterator it = m_timers.find(timerId);
    if (it == m_timers.end()) {
        qWarning() << "Timer" << timerId << "fired but there's no function mapped to it!";
        return;
    }

    // NOTE(rryan): Do not assign by reference -- make a copy. I have no idea
    // why but this causes segfaults in ~QScriptValue while scratching if we
    // don't copy here -- even though internalExecute passes the QScriptValues
    // by value. *boggle*
    const TimerInfo timerTarget = it.value();
    if (timerTarget.oneShot) {
        stopTimer(timerId);
    }

    if (timerTarget.callback.isString()) {
        internalExecute(timerTarget.context, timerTarget.callback.toString());
    } else if (timerTarget.callback.isFunction()) {
        internalExecute(timerTarget.context, timerTarget.callback);
    }
}

double ControllerEngine::getDeckRate(const QString& group) {
    double rate = 0.0;
    ControlObjectThread* pRate = getControlObjectThread(group, "rate");
    if (pRate != NULL) {
        rate = pRate->get();
    }
    ControlObjectThread* pRateDir = getControlObjectThread(group, "rate_dir");
    if (pRateDir != NULL) {
        rate *= pRateDir->get();
    }
    ControlObjectThread* pRateRange = getControlObjectThread(group, "rateRange");
    if (pRateRange != NULL) {
        rate *= pRateRange->get();
    }

    // Add 1 since the deck is playing
    rate += 1.0;

    // See if we're in reverse play
    ControlObjectThread* pReverse = getControlObjectThread(group, "reverse");
    if (pReverse != NULL && pReverse->get() == 1) {
        rate = -rate;
    }
    return rate;
}

bool ControllerEngine::isDeckPlaying(const QString& group) {
    ControlObjectThread* pPlay = getControlObjectThread(group, "play");

    if (pPlay == NULL) {
      QString error = QString("Could not getControlObjectThread()");
      scriptErrorDialog(error);
      return false;
    }

    return pPlay->get() > 0.0;
}

/* -------- ------------------------------------------------------
    Purpose: Enables scratching for relative controls
    Input:   Virtual deck to scratch,
             Number of intervals per revolution of the controller wheel,
             RPM for the track at normal speed (usually 33+1/3),
             (optional) alpha value for the filter,
             (optional) beta value for the filter
    Output:  -
    -------- ------------------------------------------------------ */
void ControllerEngine::scratchEnable(int deck, int intervalsPerRev, double rpm,
                                     double alpha, double beta, bool ramp) {

    // If we're already scratching this deck, override that with this request
    if (m_dx[deck]) {
        //qDebug() << "Already scratching deck" << deck << ". Overriding.";
        int timerId = m_scratchTimers.key(deck);
        killTimer(timerId);
        m_scratchTimers.remove(timerId);
    }

    // Controller resolution in intervals per second at normal speed.
    // (rev/min * ints/rev * mins/sec)
    double intervalsPerSecond = (rpm * intervalsPerRev) / 60.0;

    if (intervalsPerSecond == 0.0) {
        qWarning() << "Invalid rpm or intervalsPerRev supplied to scratchEnable. Ignoring request.";
        return;
    }

    m_dx[deck] = 1.0 / intervalsPerSecond;
    m_intervalAccumulator[deck] = 0.0;
    m_ramp[deck] = false;
    m_rampFactor[deck] = 0.001;
    m_brakeActive[deck] = false;

    // PlayerManager::groupForDeck is 0-indexed.
    QString group = PlayerManager::groupForDeck(deck - 1);

    // Ramp velocity, default to stopped.
    double initVelocity = 0.0;

    ControlObjectThread* pScratch2Enable =
            getControlObjectThread(group, "scratch2_enable");

    // If ramping is desired, figure out the deck's current speed
    if (ramp) {
        // See if the deck is already being scratched
        if (pScratch2Enable != NULL && pScratch2Enable->get() == 1) {
            // If so, set the filter's initial velocity to the scratch speed
            ControlObjectThread* pScratch2 =
                    getControlObjectThread(group, "scratch2");
            if (pScratch2 != NULL) {
                initVelocity = pScratch2->get();
            }
        } else if (isDeckPlaying(group)) {
            // If the deck is playing, set the filter's initial velocity to the
            // playback speed
            initVelocity = getDeckRate(group);
        }
    }

    // Initialize scratch filter
    if (alpha && beta) {
        m_scratchFilters[deck]->init(kAlphaBetaDt, initVelocity, alpha, beta);
    } else {
        // Use filter's defaults if not specified
        m_scratchFilters[deck]->init(kAlphaBetaDt, initVelocity);
    }

    // 1ms is shortest possible, OS dependent
    int timerId = startTimer(kScratchTimerMs);

    // Associate this virtual deck with this timer for later processing
    m_scratchTimers[timerId] = deck;

    // Set scratch2_enable
    if (pScratch2Enable != NULL) {
        pScratch2Enable->slotSet(1);
    }
}

/* -------- ------------------------------------------------------
    Purpose: Accumulates "ticks" of the controller wheel
    Input:   Virtual deck to scratch, interval value (usually +1 or -1)
    Output:  -
    -------- ------------------------------------------------------ */
void ControllerEngine::scratchTick(int deck, int interval) {
    m_lastMovement[deck] = Time::elapsedMsecs();
    m_intervalAccumulator[deck] += interval;
}

/* -------- ------------------------------------------------------
    Purpose: Applies the accumulated movement to the track speed
    Input:   ID of timer for this deck
    Output:  -
    -------- ------------------------------------------------------ */
void ControllerEngine::scratchProcess(int timerId) {
    int deck = m_scratchTimers[timerId];
    // PlayerManager::groupForDeck is 0-indexed.
    QString group = PlayerManager::groupForDeck(deck - 1);
    AlphaBetaFilter* filter = m_scratchFilters[deck];
    if (!filter) {
        qWarning() << "Scratch filter pointer is null on deck" << deck;
        return;
    }

    const double oldRate = filter->predictedVelocity();

    // Give the filter a data point:

    // If we're ramping to end scratching and the wheel hasn't been turned very
    // recently (spinback after lift-off,) feed fixed data
    if (m_ramp[deck] &&
        ((Time::elapsedMsecs() - m_lastMovement[deck]) > 0)) {
        filter->observation(m_rampTo[deck] * m_rampFactor[deck]);
        // Once this code path is run, latch so it always runs until reset
        //m_lastMovement[deck] += 1000;
    } else {
        // This will (and should) be 0 if no net ticks have been accumulated
        // (i.e. the wheel is stopped)
        filter->observation(m_dx[deck] * m_intervalAccumulator[deck]);
    }

    const double newRate = filter->predictedVelocity();

    // Actually do the scratching
    ControlObjectThread* pScratch2 = getControlObjectThread(group, "scratch2");
    if (pScratch2 == NULL) {
        return; // abort and maybe it'll work on the next pass
    }
    pScratch2->slotSet(newRate);

    // Reset accumulator
    m_intervalAccumulator[deck] = 0;

    // If we're ramping and the current rate is really close to the rampTo value
    // or we're in brake mode and have crossed over the zero value, end
    // scratching
    if ((m_ramp[deck] && fabs(m_rampTo[deck] - newRate) <= 0.00001) ||
        (m_brakeActive[deck] && (
            (oldRate > 0.0 && newRate < 0.0) ||
            (oldRate < 0.0 && newRate > 0.0)))) {
        // Not ramping no mo'
        m_ramp[deck] = false;

        if (m_brakeActive[deck]) {
            // If in brake mode, set scratch2 rate to 0 and turn off the play button.
            pScratch2->slotSet(0.0);
            ControlObjectThread* pPlay = getControlObjectThread(group, "play");
            if (pPlay != NULL) {
                pPlay->slotSet(0.0);
            }
        }

        // Clear scratch2_enable to end scratching.
        ControlObjectThread* pScratch2Enable =
                getControlObjectThread(group, "scratch2_enable");
        if (pScratch2Enable == NULL) {
            return; // abort and maybe it'll work on the next pass
        }
        pScratch2Enable->slotSet(0);

        // Remove timer
        killTimer(timerId);
        m_scratchTimers.remove(timerId);

        m_dx[deck] = 0.0;
        m_brakeActive[deck] = false;
    }
}

/* -------- ------------------------------------------------------
    Purpose: Stops scratching the specified virtual deck
    Input:   Virtual deck to stop scratching
    Output:  -
    -------- ------------------------------------------------------ */
void ControllerEngine::scratchDisable(int deck, bool ramp) {
    // PlayerManager::groupForDeck is 0-indexed.
    QString group = PlayerManager::groupForDeck(deck - 1);

    m_rampTo[deck] = 0.0;

    // If no ramping is desired, disable scratching immediately
    if (!ramp) {
        // Clear scratch2_enable
        ControlObjectThread* pScratch2Enable = getControlObjectThread(group, "scratch2_enable");
        if (pScratch2Enable != NULL) {
            pScratch2Enable->slotSet(0);
        }
        // Can't return here because we need scratchProcess to stop the timer.
        // So it's still actually ramping, we just won't hear or see it.
    } else if (isDeckPlaying(group)) {
        // If so, set the target velocity to the playback speed
        m_rampTo[deck] = getDeckRate(group);
    }

    m_lastMovement[deck] = Time::elapsedMsecs();
    m_ramp[deck] = true;    // Activate the ramping in scratchProcess()
}

/* -------- ------------------------------------------------------
    Purpose: Tells if the specified deck is currently scratching
             (Scripts need this to implement spinback-after-lift-off)
    Input:   Virtual deck to inquire about
    Output:  True if so
    -------- ------------------------------------------------------ */
bool ControllerEngine::isScratching(int deck) {
    // PlayerManager::groupForDeck is 0-indexed.
    QString group = PlayerManager::groupForDeck(deck - 1);
    // Don't report that we are scratching if we're ramping.
    return getValue(group, "scratch2_enable") > 0 && !m_ramp[deck];
}

/*  -------- ------------------------------------------------------
    Purpose: [En/dis]ables soft-takeover status for a particular ControlObject
    Input:   ControlObject group and key values,
                whether to set the soft-takeover status or not
    Output:  -
    -------- ------------------------------------------------------ */
void ControllerEngine::softTakeover(QString group, QString name, bool set) {
    ControlObject* pControl = ControlObject::getControl(ConfigKey(group, name));
    if (!pControl) {
        return;
    }
    if (set) {
        m_st.enable(pControl);
    } else {
        m_st.disable(pControl);
    }
}

/*  -------- ------------------------------------------------------
     Purpose: Ignores the next value for the given ControlObject
                This is used when an absolute physical control is changed to
                to operate on a different ControlObject, allowing it to sync up
                to the soft-takeover state without an abrupt jump.
     Input:   ControlObject group and key values
     Output:  -
     -------- ------------------------------------------------------ */
void ControllerEngine::softTakeoverIgnoreNextValue(QString group, QString name) {
    ControlObject* pControl = ControlObject::getControl(ConfigKey(group, name));
    if (!pControl) {
        return;
    }
    
    m_st.ignoreNext(pControl);
}

/*  -------- ------------------------------------------------------
    Purpose: [En/dis]ables spinback effect for the channel
    Input:   deck, activate/deactivate, factor (optional),
             delay (optional), rate (optional)
    Output:  -
    -------- ------------------------------------------------------ */
void ControllerEngine::spinback(int deck, bool activate, double factor, double rate) {
    // defaults for args set in header file
    brake(deck, activate, factor, rate);
}

/*  -------- ------------------------------------------------------
    Purpose: [En/dis]ables brake/spinback effect for the channel
    Input:   deck, activate/deactivate, factor (optional),
             delay (optional), rate (optional)
    Output:  -
    -------- ------------------------------------------------------ */
void ControllerEngine::brake(int deck, bool activate, double factor, double rate) {
    // PlayerManager::groupForDeck is 0-indexed.
    QString group = PlayerManager::groupForDeck(deck - 1);

    // kill timer when both enabling or disabling
    int timerId = m_scratchTimers.key(deck);
    killTimer(timerId);
    m_scratchTimers.remove(timerId);

    // enable/disable scratch2 mode
    ControlObjectThread* pScratch2Enable = getControlObjectThread(group, "scratch2_enable");
    if (pScratch2Enable != NULL) {
        pScratch2Enable->slotSet(activate ? 1 : 0);
    }

    // used in scratchProcess for the different timer behavior we need
    m_brakeActive[deck] = activate;

    if (activate) {
        // store the new values for this spinback/brake effect
        m_rampFactor[deck] = rate * factor / 100000.0; // approx 1 second for a factor of 1
        m_rampTo[deck] = 0.0;

        // setup timer and set scratch2
        int timerId = startTimer(kScratchTimerMs);
        m_scratchTimers[timerId] = deck;

        ControlObjectThread* pScratch2 = getControlObjectThread(group, "scratch2");
        if (pScratch2 != NULL) {
            pScratch2->slotSet(rate);
        }

        // setup the filter using the default values of alpha and beta
        AlphaBetaFilter* filter = m_scratchFilters[deck];
        if (filter != NULL) {
            filter->init(kAlphaBetaDt, rate);
        }

        // activate the ramping in scratchProcess()
        m_ramp[deck] = true;
    }
}
