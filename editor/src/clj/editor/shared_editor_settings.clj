;; Copyright 2020-2022 The Defold Foundation
;; Copyright 2014-2020 King
;; Copyright 2009-2014 Ragnar Svensson, Christian Murray
;; Licensed under the Defold License version 1.0 (the "License"); you may not use
;; this file except in compliance with the License.
;;
;; You may obtain a copy of the License, together with FAQs at
;; https://www.defold.com/license
;;
;; Unless required by applicable law or agreed to in writing, software distributed
;; under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
;; CONDITIONS OF ANY KIND, either express or implied. See the License for the
;; specific language governing permissions and limitations under the License.

(ns editor.shared-editor-settings
  (:require [cljfx.fx.v-box :as fx.v-box]
            [clojure.java.io :as io]
            [editor.dialogs :as dialogs]
            [editor.fxui :as fxui]
            [editor.settings :as settings]
            [editor.settings-core :as settings-core]
            [editor.ui :as ui]
            [service.log :as log])
  (:import [java.io File]))

(set! *warn-on-reflection* true)

(def ^:private project-shared-editor-settings-filename "project.shared_editor_settings")

(def project-shared-editor-settings-proj-path (str \/ project-shared-editor-settings-filename))

(def ^:private shared-editor-settings-icon "icons/32/Icons_05-Project-info.png")

(def ^:private shared-editor-settings-meta "shared-editor-settings-meta.edn")

(def ^:private shared-editor-settings-meta-info
  (delay
    (with-open [resource-reader (settings-core/resource-reader shared-editor-settings-meta)
                pushback-reader (settings-core/pushback-reader resource-reader)]
      (settings-core/load-meta-info pushback-reader))))

(defn register-resource-types [workspace]
  (settings/register-simple-settings-resource-type workspace
    :ext "shared_editor_settings"
    :label "Shared Editor Settings"
    :icon shared-editor-settings-icon
    :meta-info @shared-editor-settings-meta-info))

(defn- report-load-error! [^File shared-editor-settings-file ^Throwable exception]
  (let [header-message "Failed to load Shared Editor Settings file."
        sub-header-message "Falling back to defaults. Your user experience might suffer."
        file-info-message (str "File: " (.getAbsolutePath shared-editor-settings-file))
        exception-message (ex-message exception)]
    (log/warn :msg (str header-message " " sub-header-message " " file-info-message)
              :exception exception)
    (ui/run-later
      (dialogs/make-info-dialog
        {:title "Error Loading Shared Editor Settings"
         :icon :icon/triangle-error
         :always-on-top true
         :header {:fx/type fx.v-box/lifecycle
                  :children [{:fx/type fxui/label
                              :variant :header
                              :text header-message}
                             {:fx/type fxui/label
                              :text sub-header-message}]}
         :content (str file-info-message "\n\n" exception-message)}))))

(defn- load-config [^File shared-editor-settings-file parse-config-fn]
  (let [raw-settings-or-exception (try
                                    (with-open [reader (io/reader shared-editor-settings-file)]
                                      (settings-core/parse-settings reader))
                                    (catch Exception exception
                                      exception))]
    (if (ex-message raw-settings-or-exception)
      (report-load-error! shared-editor-settings-file raw-settings-or-exception)
      (let [meta-settings (:settings @shared-editor-settings-meta-info)
            config-or-exception (try
                                  (let [settings (settings-core/sanitize-settings meta-settings raw-settings-or-exception)]
                                    (parse-config-fn settings))
                                  (catch Exception exception
                                    exception))]
        (if (ex-message config-or-exception)
          (report-load-error! shared-editor-settings-file config-or-exception)
          (when (not-empty config-or-exception)
            config-or-exception))))))

(defn- load-project-config [project-directory config-type parse-config-fn]
  {:pre [(keyword? config-type)
         (ifn? parse-config-fn)]}
  (let [shared-editor-settings-file (io/file project-directory project-shared-editor-settings-filename)]
    (when (.isFile shared-editor-settings-file)
      (log/info :message (str "Loading " (name config-type) " from Shared Editor Settings file."))
      (when-some [config (not-empty (load-config shared-editor-settings-file parse-config-fn))]
        (log/info :message (str "Using " (name config-type) " from Shared Editor Settings file.") config-type config)
        config))))

(defn- parse-system-config [settings]
  (let [cache-capacity (settings-core/get-setting settings ["performance" "cache_capacity"])]
    (when (some? cache-capacity)
      (if (<= -1 cache-capacity)
        {:cache-size cache-capacity}
        (throw (ex-info "performance.cache_capacity must be -1 (unlimited), 0 (disabled), or positive."
                        {:cache-capacity cache-capacity}))))))

(defn non-editable-directory-proj-path? [value]
  ;; Value must be a string starting with "/", but not ending with "/".
  (and (string? value)
       (re-matches #"^\/.*(?<!\/)$" value)))

(defn- parse-workspace-config [settings]
  (let [non-editable-directories (settings-core/get-setting settings ["performance" "non_editable_directories"])]
    (when (seq non-editable-directories)
      (if (every? non-editable-directory-proj-path? non-editable-directories)
        {:non-editable-directories non-editable-directories}
        (throw (ex-info "Every entry in performance.non_editable_directories must be a proj-path starting with `/`, but not ending with `/`."
                        {:non-editable-directories non-editable-directories}))))))

(def ^:private default-system-config {})

(def ^:private default-workspace-config {})

;; Called through reflection.
(defn load-project-system-config [project-directory]
  (or (load-project-config project-directory :system-config parse-system-config)
      default-system-config))

(defn load-project-workspace-config [project-directory]
  (or (load-project-config project-directory :workspace-config parse-workspace-config)
      default-workspace-config))
