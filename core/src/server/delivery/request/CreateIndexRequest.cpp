// Copyright (C) 2019-2020 Zilliz. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except in compliance
// with the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied. See the License for the specific language governing permissions and limitations under the License.

#include "server/delivery/request/CreateIndexRequest.h"
#include "db/SnapshotUtils.h"
#include "db/Utils.h"
#include "server/DBWrapper.h"
#include "server/ValidationUtil.h"
#include "utils/Log.h"
#include "utils/TimeRecorder.h"

#include <fiu-local.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace milvus {
namespace server {

CreateIndexRequest::CreateIndexRequest(const std::shared_ptr<milvus::server::Context>& context,
                                       const std::string& collection_name, const std::string& field_name,
                                       const std::string& index_name, const milvus::json& json_params)
    : BaseRequest(context, BaseRequest::kCreateIndex),
      collection_name_(collection_name),
      field_name_(field_name),
      index_name_(index_name),
      json_params_(json_params) {
}

BaseRequestPtr
CreateIndexRequest::Create(const std::shared_ptr<milvus::server::Context>& context, const std::string& collection_name,
                           const std::string& field_name, const std::string& index_name,
                           const milvus::json& json_params) {
    return std::shared_ptr<BaseRequest>(
        new CreateIndexRequest(context, collection_name, field_name, index_name, json_params));
}

Status
CreateIndexRequest::OnExecute() {
    try {
        std::string hdr = "CreateIndexRequest(collection=" + collection_name_ + ")";
        TimeRecorderAuto rc(hdr);

        // step 1: check arguments
        auto status = ValidateCollectionName(collection_name_);
        if (!status.ok()) {
            return status;
        }

        status = ValidateFieldName(field_name_);
        if (!status.ok()) {
            return status;
        }

        status = ValidateIndexName(index_name_);
        if (!status.ok()) {
            return status;
        }

        // only process root collection, ignore partition collection
        engine::snapshot::CollectionPtr collection;
        engine::snapshot::CollectionMappings fields_schema;
        status = DBWrapper::DB()->DescribeCollection(collection_name_, collection, fields_schema);
        if (!status.ok()) {
            if (status.code() == DB_NOT_FOUND) {
                return Status(SERVER_COLLECTION_NOT_EXIST, CollectionNotExistMsg(collection_name_));
            } else {
                return status;
            }
        }

        // pick up field
        engine::snapshot::FieldPtr field;
        for (auto field_it = fields_schema.begin(); field_it != fields_schema.end(); field_it++) {
            if (field_it->first->GetName() == field_name_) {
                field = field_it->first;
                break;
            }
        }
        if (field == nullptr) {
            return Status(SERVER_INVALID_FIELD_NAME, "Invalid field name");
        }

        engine::CollectionIndex index;
        if (engine::IsVectorField(field)) {
            int32_t field_type = field->GetFtype();
            auto params = field->GetParams();
            int64_t dimension = params[engine::DIMENSION].get<int64_t>();

            // validate index type
            std::string index_type = 0;
            if (json_params_.contains("index_type")) {
                index_type = json_params_["index_type"].get<std::string>();
            }
            status = ValidateIndexType(index_type);
            if (!status.ok()) {
                return status;
            }

            // validate metric type
            std::string metric_type = 0;
            if (json_params_.contains("metric_type")) {
                metric_type = json_params_["metric_type"].get<std::string>();
            }
            status = ValidateMetricType(metric_type);
            if (!status.ok()) {
                return status;
            }

            // validate index parameters
            status = ValidateIndexParams(json_params_, dimension, index_type);
            if (!status.ok()) {
                return status;
            }

            rc.RecordSection("check validation");

            index.index_name_ = index_name_;
            index.metric_name_ = metric_type;
            index.extra_params_ = json_params_;
        } else {
            index.index_name_ = index_name_;
        }

        status = DBWrapper::DB()->CreateIndex(context_, collection_name_, field_name_, index);
        if (!status.ok()) {
            return status;
        }
    } catch (std::exception& ex) {
        return Status(SERVER_UNEXPECTED_ERROR, ex.what());
    }

    return Status::OK();
}

}  // namespace server
}  // namespace milvus
