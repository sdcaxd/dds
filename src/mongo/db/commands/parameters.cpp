// parameters.cpp

/**
*    Copyright (C) 2012 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "mongo/platform/basic.h"

#include <boost/algorithm/string.hpp>
#include <set>

#include "mongo/bson/json.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/config.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/internal_user_auth.h"
#include "mongo/db/command_generic_argument.h"
#include "mongo/db/commands.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/logger/logger.h"
#include "mongo/logger/parse_log_component_settings.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/util/mongoutils/str.h"

using std::string;
using std::stringstream;

namespace mongo {

namespace {
void appendParameterNames(std::string* help) {
    *help += "supported:\n";
    for (const auto& kv : ServerParameterSet::getGlobal()->getMap()) {
        *help += "  ";
        *help += kv.first;
        *help += '\n';
    }
}
}  // namespace

class CmdGet : public ErrmsgCommandDeprecated {
public:
    CmdGet() : ErrmsgCommandDeprecated("getParameter") {}
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    virtual bool adminOnly() const {
        return true;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {
        ActionSet actions;
        actions.addAction(ActionType::getParameter);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }
    std::string help() const override {
        std::string h =
            "get administrative option(s)\nexample:\n"
            "{ getParameter:1, notablescan:1 }\n";
        appendParameterNames(&h);
        h += "{ getParameter:'*' } to get everything\n";
        return h;
    }
    bool errmsgRun(OperationContext* opCtx,
                   const string& dbname,
                   const BSONObj& cmdObj,
                   string& errmsg,
                   BSONObjBuilder& result) {
        bool all = *cmdObj.firstElement().valuestrsafe() == '*';

        int before = result.len();

        const ServerParameter::Map& m = ServerParameterSet::getGlobal()->getMap();
        for (ServerParameter::Map::const_iterator i = m.begin(); i != m.end(); ++i) {
            if (all || cmdObj.hasElement(i->first.c_str())) {
                i->second->append(opCtx, result, i->second->name());
            }
        }

        if (before == result.len()) {
            errmsg = "no option found to get";
            return false;
        }
        return true;
    }
} cmdGet;

class CmdSet : public ErrmsgCommandDeprecated {
public:
    CmdSet() : ErrmsgCommandDeprecated("setParameter") {}
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }
    virtual bool adminOnly() const {
        return true;
    }
    virtual bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }
    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) const {
        ActionSet actions;
        actions.addAction(ActionType::setParameter);
        out->push_back(Privilege(ResourcePattern::forClusterResource(), actions));
    }
    std::string help() const override {
        std::string h =
            "set administrative option(s)\n"
            "{ setParameter:1, <param>:<value> }\n";
        appendParameterNames(&h);
        return h;
    }
    bool errmsgRun(OperationContext* opCtx,
                   const string& dbname,
                   const BSONObj& cmdObj,
                   string& errmsg,
                   BSONObjBuilder& result) {
        int numSet = 0;
        bool found = false;

        const ServerParameter::Map& parameterMap = ServerParameterSet::getGlobal()->getMap();

        // First check that we aren't setting the same parameter twice and that we actually are
        // setting parameters that we have registered and can change at runtime
        BSONObjIterator parameterCheckIterator(cmdObj);

        // We already know that "setParameter" will be the first element in this object, so skip
        // past that
        parameterCheckIterator.next();

        // Set of all the parameters the user is attempting to change
        std::map<std::string, BSONElement> parametersToSet;

        // Iterate all parameters the user passed in to do the initial validation checks,
        // including verifying that we are not setting the same parameter twice.
        while (parameterCheckIterator.more()) {
            BSONElement parameter = parameterCheckIterator.next();
            std::string parameterName = parameter.fieldName();
            if (isGenericArgument(parameterName))
                continue;

            ServerParameter::Map::const_iterator foundParameter = parameterMap.find(parameterName);

            // Check to see if this is actually a valid parameter
            if (foundParameter == parameterMap.end()) {
                errmsg = str::stream() << "attempted to set unrecognized parameter ["
                                       << parameterName << "], use help:true to see options ";
                return false;
            }

            // Make sure we are allowed to change this parameter
            if (!foundParameter->second->allowedToChangeAtRuntime()) {
                errmsg = str::stream() << "not allowed to change [" << parameterName
                                       << "] at runtime";
                return false;
            }

            // Make sure we are only setting this parameter once
            if (parametersToSet.count(parameterName)) {
                errmsg = str::stream()
                    << "attempted to set parameter [" << parameterName
                    << "] twice in the same setParameter command, "
                    << "once to value: [" << parametersToSet[parameterName].toString(false)
                    << "], and once to value: [" << parameter.toString(false) << "]";
                return false;
            }

            parametersToSet[parameterName] = parameter;
        }

        // Iterate the parameters that we have confirmed we are setting and actually set them.
        // Not that if setting any one parameter fails, the command will fail, but the user
        // won't see what has been set and what hasn't.  See SERVER-8552.
        for (std::map<std::string, BSONElement>::iterator it = parametersToSet.begin();
             it != parametersToSet.end();
             ++it) {
            BSONElement parameter = it->second;
            std::string parameterName = it->first;

            ServerParameter::Map::const_iterator foundParameter = parameterMap.find(parameterName);

            if (foundParameter == parameterMap.end()) {
                errmsg = str::stream() << "Parameter: " << parameterName << " that was "
                                       << "avaliable during our first lookup in the registered "
                                       << "parameters map is no longer available.";
                return false;
            }

            if (numSet == 0) {
                foundParameter->second->append(opCtx, result, "was");
            }

            uassertStatusOK(foundParameter->second->set(parameter));
            numSet++;
        }

        if (numSet == 0 && !found) {
            errmsg = "no option found to set, use help:true to see options ";
            return false;
        }

        return true;
    }
} cmdSet;

namespace {
using logger::globalLogDomain;
using logger::LogComponent;
using logger::LogComponentSetting;
using logger::LogSeverity;
using logger::parseLogComponentSettings;

class LogLevelSetting : public ServerParameter {
public:
    LogLevelSetting() : ServerParameter(ServerParameterSet::getGlobal(), "logLevel") {}

    virtual void append(OperationContext* opCtx, BSONObjBuilder& b, const std::string& name) {
        b << name << globalLogDomain()->getMinimumLogSeverity().toInt();
    }

    virtual Status set(const BSONElement& newValueElement) {
        int newValue;
        if (!newValueElement.coerce(&newValue) || newValue < 0)
            return Status(ErrorCodes::BadValue,
                          mongoutils::str::stream() << "Invalid value for logLevel: "
                                                    << newValueElement);
        LogSeverity newSeverity =
            (newValue > 0) ? LogSeverity::Debug(newValue) : LogSeverity::Log();
        globalLogDomain()->setMinimumLoggedSeverity(newSeverity);
        return Status::OK();
    }

    virtual Status setFromString(const std::string& str) {
        int newValue;
        Status status = parseNumberFromString(str, &newValue);
        if (!status.isOK())
            return status;
        if (newValue < 0)
            return Status(ErrorCodes::BadValue,
                          mongoutils::str::stream() << "Invalid value for logLevel: " << newValue);
        LogSeverity newSeverity =
            (newValue > 0) ? LogSeverity::Debug(newValue) : LogSeverity::Log();
        globalLogDomain()->setMinimumLoggedSeverity(newSeverity);
        return Status::OK();
    }
} logLevelSetting;

/**
 * Log component verbosity.
 * Log levels of log component hierarchy.
 * Negative value for a log component means the default log level will be used.
 */
class LogComponentVerbositySetting : public ServerParameter {
    MONGO_DISALLOW_COPYING(LogComponentVerbositySetting);

public:
    LogComponentVerbositySetting()
        : ServerParameter(ServerParameterSet::getGlobal(), "logComponentVerbosity") {}

    virtual void append(OperationContext* opCtx, BSONObjBuilder& b, const std::string& name) {
        BSONObj currentSettings;
        _get(&currentSettings);
        b << name << currentSettings;
    }

    virtual Status set(const BSONElement& newValueElement) {
        if (!newValueElement.isABSONObj()) {
            return Status(ErrorCodes::TypeMismatch,
                          mongoutils::str::stream()
                              << "log component verbosity is not a BSON object: "
                              << newValueElement);
        }
        return _set(newValueElement.Obj());
    }

    virtual Status setFromString(const std::string& str) {
        try {
            return _set(mongo::fromjson(str));
        } catch (const DBException& ex) {
            return ex.toStatus();
        }
    }

private:
    /**
     * Returns current settings as a BSON document.
     * The "default" log component is an implementation detail. Don't expose this to users.
     */
    void _get(BSONObj* output) const {
        static const string defaultLogComponentName =
            LogComponent(LogComponent::kDefault).getShortName();

        mutablebson::Document doc;

        for (int i = 0; i < int(LogComponent::kNumLogComponents); ++i) {
            LogComponent component = static_cast<LogComponent::Value>(i);

            int severity = -1;
            if (globalLogDomain()->hasMinimumLogSeverity(component)) {
                severity = globalLogDomain()->getMinimumLogSeverity(component).toInt();
            }

            // Save LogComponent::kDefault LogSeverity at root
            if (component == LogComponent::kDefault) {
                doc.root().appendInt("verbosity", severity).transitional_ignore();
                continue;
            }

            mutablebson::Element element = doc.makeElementObject(component.getShortName());
            element.appendInt("verbosity", severity).transitional_ignore();

            mutablebson::Element parentElement = _getParentElement(doc, component);
            parentElement.pushBack(element).transitional_ignore();
        }

        BSONObj result = doc.getObject();
        output->swap(result);
        invariant(!output->hasField(defaultLogComponentName));
    }

    /**
     * Updates component hierarchy log levels.
     *
     * BSON Format:
     * {
     *     verbosity: 4,  <-- maps to 'default' log component.
     *     componentA: {
     *         verbosity: 2,  <-- sets componentA's log level to 2.
     *         componentB: {
     *             verbosity: 1, <-- sets componentA.componentB's log level to 1.
     *         }
     *         componentC: {
     *             verbosity: -1, <-- clears componentA.componentC's log level so that
     *                                its final loglevel will be inherited from componentA.
     *         }
     *     },
     *     componentD : 3  <-- sets componentD's log level to 3 (alternative to
     *                         subdocument with 'verbosity' field).
     * }
     *
     * For the default component, the log level is read from the top-level
     * "verbosity" field.
     * For non-default components, we look up the element using the component's
     * dotted name. If the "<dotted component name>" field is a number, the log
     * level will be read from the field's value.
     * Otherwise, we assume that the "<dotted component name>" field is an
     * object with a "verbosity" field that holds the log level for the component.
     * The more verbose format with the "verbosity" field is intended to support
     * setting of log levels of both parent and child log components in the same
     * BSON document.
     *
     * Ignore elements in BSON object that do not map to a log component's dotted
     * name.
     */
    Status _set(const BSONObj& bsonSettings) const {
        StatusWith<std::vector<LogComponentSetting>> parseStatus =
            parseLogComponentSettings(bsonSettings);

        if (!parseStatus.isOK()) {
            return parseStatus.getStatus();
        }

        std::vector<LogComponentSetting> settings = parseStatus.getValue();
        std::vector<LogComponentSetting>::iterator it = settings.begin();
        for (; it < settings.end(); ++it) {
            LogComponentSetting newSetting = *it;

            // Negative value means to clear log level of component.
            if (newSetting.level < 0) {
                globalLogDomain()->clearMinimumLoggedSeverity(newSetting.component);
                continue;
            }
            // Convert non-negative value to Log()/Debug(N).
            LogSeverity newSeverity =
                (newSetting.level > 0) ? LogSeverity::Debug(newSetting.level) : LogSeverity::Log();
            globalLogDomain()->setMinimumLoggedSeverity(newSetting.component, newSeverity);
        }

        return Status::OK();
    }

    /**
     * Search document for element corresponding to log component's parent.
     */
    static mutablebson::Element _getParentElement(mutablebson::Document& doc,
                                                  LogComponent component) {
        // Hide LogComponent::kDefault
        if (component == LogComponent::kDefault) {
            return doc.end();
        }
        LogComponent parentComponent = component.parent();

        // Attach LogComponent::kDefault children to root
        if (parentComponent == LogComponent::kDefault) {
            return doc.root();
        }
        mutablebson::Element grandParentElement = _getParentElement(doc, parentComponent);
        return grandParentElement.findFirstChildNamed(parentComponent.getShortName());
    }
} logComponentVerbositySetting;

class ReadOnlySetting : public ServerParameter {
public:
    ReadOnlySetting() : ServerParameter(ServerParameterSet::getGlobal(), "readOnly") {}

    virtual void append(OperationContext* txn, BSONObjBuilder& b, const std::string& name) {
        b << name << serverGlobalParams.readOnly;
    }

    virtual Status set(const BSONElement& newValueElement) {
        if (newValueElement.type() != BSONType::Bool) {
            return Status(ErrorCodes::BadValue, str::stream() << "Invalid value type for readOnly");
        }

        serverGlobalParams.readOnly = newValueElement.Bool();
        return Status::OK();
    }

    virtual Status setFromString(const std::string& str) {
        bool newValue = true;
        if (str == "true") {
            newValue = true;
        } else if (str == "false") {
            newValue = false;
        } else {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "Invalid value type for readOnly: " << str);
        }

        serverGlobalParams.readOnly = newValue;
        return Status::OK();
    }
} readOnlySetting;


ExportedServerParameter<bool, ServerParameterType::kStartupAndRuntime> QuietSetting(
    ServerParameterSet::getGlobal(), "quiet", &serverGlobalParams.quiet);

ExportedServerParameter<bool, ServerParameterType::kRuntimeOnly> TraceExceptionsSetting(
    ServerParameterSet::getGlobal(), "traceExceptions", &DBException::traceExceptions);

class AutomationServiceDescriptor final : public ServerParameter {
public:
    static constexpr auto kName = "automationServiceDescriptor"_sd;
    static constexpr auto kMaxSize = 64U;

    AutomationServiceDescriptor()
        : ServerParameter(ServerParameterSet::getGlobal(), kName.toString(), true, true) {}

    virtual void append(OperationContext* opCtx,
                        BSONObjBuilder& builder,
                        const std::string& name) override {
        const stdx::lock_guard<stdx::mutex> lock(_mutex);
        if (!_value.empty())
            builder << name << _value;
    }

    virtual Status set(const BSONElement& newValueElement) override {
        if (newValueElement.type() != mongo::String)
            return {ErrorCodes::TypeMismatch,
                    mongoutils::str::stream() << "Value for parameter " << kName
                                              << " must be of type 'string'"};
        return setFromString(newValueElement.String());
    }

    virtual Status setFromString(const std::string& str) override {
        if (str.size() > kMaxSize)
            return {ErrorCodes::Overflow,
                    mongoutils::str::stream() << "Value for parameter " << kName
                                              << " must be no more than "
                                              << kMaxSize
                                              << " bytes"};

        {
            const stdx::lock_guard<stdx::mutex> lock(_mutex);
            _value = str;
        }

        return Status::OK();
    }

private:
    stdx::mutex _mutex;
    std::string _value;
} automationServiceDescriptor;

//添加 isImplicitCreateCol
class IsImplicitCreateColSetting : public ServerParameter {
public:
    IsImplicitCreateColSetting()
        : ServerParameter(ServerParameterSet::getGlobal(), "isImplicitCreateCol") {}

    virtual void append(OperationContext* txn, BSONObjBuilder& b, const std::string& name) {
        b << name << serverGlobalParams.isImplicitCreateCol;
    }

    virtual Status set(const BSONElement& newValueElement) {
        if (newValueElement.type() != BSONType::Bool) {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "Invalid value type for isImplicitCreateCol");
        }

        serverGlobalParams.isImplicitCreateCol = newValueElement.Bool();
        return Status::OK();
    }

    virtual Status setFromString(const std::string& str) {
        bool newValue = true;
        if (str == "true") {
            newValue = true;
        } else if (str == "false") {
            newValue = false;
        } else {
            return Status(ErrorCodes::BadValue,
                          str::stream() << "Invalid value type for isImplicitCreateCol: " << str);
        }

        serverGlobalParams.isImplicitCreateCol = newValue;
        return Status::OK();
    }
} isImplicitCreateCol;

class MaxIncomingConnectionsSetting : public ServerParameter {
public:
    MaxIncomingConnectionsSetting()
        : ServerParameter(ServerParameterSet::getGlobal(), "maxIncomingConnections") {}

    virtual void append(OperationContext* txn, BSONObjBuilder& b, const std::string& name) {
        b << name << static_cast<int>(serverGlobalParams.maxConns);
    }

    virtual Status set(const BSONElement& newValueElement) {
        int newValue = 0;
        if (!newValueElement.coerce(&newValue) || newValue < 0)
            return Status(
                ErrorCodes::BadValue,
                mongoutils::str::stream()
                    << "Invalid value for maxIncomingConnections: "
                    << newValueElement
                    << " and now MaxConnection is "
                    << getGlobalServiceContext()->getServiceEntryPoint()->getMaxNumConnections());
        // invalid use of incomplete type 'class mongo::ServiceEntryPoint'
        // return a serviceContext
        Status status =
            getGlobalServiceContext()->getServiceEntryPoint()->setMaxNumConnections(newValue);

        if (!status.isOK())
            return status;
        serverGlobalParams.maxConns = newValue;
        return Status::OK();
    }


    virtual Status setFromString(const std::string& str) {
        int newValue = 0;
        Status status = parseNumberFromString(str, &newValue);
        if (!status.isOK())
            return status;
        if (newValue < 0)
            return Status(
                ErrorCodes::BadValue,
                mongoutils::str::stream()
                    << "Invalid value for maxIncomingConnections: "
                    << newValue
                    << " and now MaxConnection is "
                    << getGlobalServiceContext()->getServiceEntryPoint()->getMaxNumConnections());

        status = getGlobalServiceContext()->getServiceEntryPoint()->setMaxNumConnections(newValue);
        if (!status.isOK())
            return status;
        serverGlobalParams.maxConns = newValue;
        return Status::OK();
    }
} maxIncomingConnectionsSetting;

class AllowCommandSetting : public ServerParameter {
public:
    AllowCommandSetting() : ServerParameter(ServerParameterSet::getGlobal(), "allowCommands") {}

    virtual void append(OperationContext* txn, BSONObjBuilder& b, const std::string& name) {
        b << name << boost::algorithm::join(serverGlobalParams.allowCommands, ",");
    }

    virtual Status set(const BSONElement& newValueElement) {
        try {
            return setFromString(newValueElement.String());
        } catch (const DBException& msg) {
            return Status(ErrorCodes::BadValue,
                          mongoutils::str::stream() << "Invalid parameter for allowCommands: "
                                                    << newValueElement
                                                    << ", exception: "
                                                    << msg.what());
        }
    }

    virtual Status setFromString(const std::string& str) {

        auto allowcommands_temp = std::vector<std::string>();
        boost::split(allowcommands_temp, str, boost::is_any_of(", "));
        for (const auto command : allowcommands_temp) {
            if (Command::globleDisableCommands.find(command) ==
                Command::globleDisableCommands.end()) {
                return Status(ErrorCodes::BadValue,
                              mongoutils::str::stream() << command << " is not a disable command.");
            }
        }
        serverGlobalParams.allowCommands = allowcommands_temp;  // set command override yaml config
        return Status::OK();
    }
} allowCommandSetting;

class MaxInternalIncomingConnectionsSetting : public ServerParameter {
public:
    MaxInternalIncomingConnectionsSetting()
        : ServerParameter(ServerParameterSet::getGlobal(), "maxInternalIncomingConnections") {}

    virtual void append(OperationContext* txn, BSONObjBuilder& b, const std::string& name) {
        b << name << serverGlobalParams.maxInternalConns;
    }

    virtual Status set(const BSONElement& newValueElement) {
        int newValue = 0;
        if (!newValueElement.coerce(&newValue) || newValue < 0)
            return Status(ErrorCodes::BadValue,
                          mongoutils::str::stream()
                              << "Invalid value for maxInternalIncomingConnections: "
                              << newValueElement
                              << " and now MaxInternalConnection is "
                              << getGlobalServiceContext()
                                     ->getServiceEntryPoint()
                                     ->getMaxNumInternalConnections());
        Status status =
            getGlobalServiceContext()->getServiceEntryPoint()->setMaxNumInternalConnections(
                newValue);
        if (!status.isOK())
            return status;
        serverGlobalParams.maxInternalConns = newValue;
        return Status::OK();
    }

    virtual Status setFromString(const std::string& str) {
        int newValue = 0;
        Status status = parseNumberFromString(str, &newValue);
        if (!status.isOK())
            return status;
        if (newValue < 0)
            return Status(ErrorCodes::BadValue,
                          mongoutils::str::stream()
                              << "Invalid value for maxInternalIncomingConnections: "
                              << newValue
                              << " and now MaxInternalConnection is "
                              << getGlobalServiceContext()
                                     ->getServiceEntryPoint()
                                     ->getMaxNumInternalConnections());
        status = getGlobalServiceContext()->getServiceEntryPoint()->setMaxNumInternalConnections(
            newValue);
        if (!status.isOK())
            return status;
        serverGlobalParams.maxInternalConns = newValue;
        return Status::OK();
    }
} maxInternalIncomingConnections;

constexpr decltype(AutomationServiceDescriptor::kName) AutomationServiceDescriptor::kName;
constexpr decltype(AutomationServiceDescriptor::kMaxSize) AutomationServiceDescriptor::kMaxSize;

}  // namespace
}  // namespace mongo
