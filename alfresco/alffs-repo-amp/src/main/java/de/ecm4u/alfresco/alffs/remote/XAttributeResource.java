package de.ecm4u.alfresco.alffs.remote;

import java.io.IOException;
import java.io.Serializable;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedList;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.regex.Matcher;

import org.alfresco.model.ContentModel;
import org.alfresco.repo.policy.BehaviourFilter;
import org.alfresco.service.cmr.dictionary.DataTypeDefinition;
import org.alfresco.service.cmr.dictionary.PropertyDefinition;
import org.alfresco.service.cmr.repository.ContentData;
import org.alfresco.service.cmr.repository.NodeRef;
import org.alfresco.service.namespace.QName;
import org.apache.commons.lang.StringUtils;
import org.json.JSONException;
import org.json.simple.JSONValue;
import org.springframework.extensions.webscripts.Status;
import org.springframework.extensions.webscripts.WebScriptRequest;
import org.springframework.extensions.webscripts.WebScriptResponse;

public class XAttributeResource extends AbstractIOResource {
    private BehaviourFilter behaviourFilter;

    @Override
    public void doGet(WebScriptRequest req, WebScriptResponse res) throws IOException, JSONException {
        res.setContentType(JSON_MIMETYPE);
        res.setContentEncoding(DEFAULT_ENCODING);
        res.setStatus(Status.STATUS_OK);

        String base = req.getParameter(PARAM_BASE);
        String path = req.getParameter(PARAM_PATH);
        String key = req.getParameter(PARAM_KEY);

        if (StringUtils.isBlank(base) || StringUtils.isBlank(path)) {
            res.setStatus(Status.STATUS_BAD_REQUEST);
            Map<String, Object> jsonResponse = new HashMap<String, Object>();
            jsonResponse.put("message", "garbage in garbage out");
            jsonResponse.put("errno", "EIO");
            JSONValue.writeJSONString(jsonResponse, res.getWriter());
            return;
        }

        NodeRef nodeRef = locateNode(base, path);
        if (nodeRef == null) {
            res.setStatus(Status.STATUS_NOT_FOUND);
            Map<String, Object> jsonResponse = new HashMap<String, Object>();
            jsonResponse.put("message", "no such file or directory");
            jsonResponse.put("errno", "ENOENT");
            JSONValue.writeJSONString(jsonResponse, res.getWriter());
            return;
        }

        if (StringUtils.isBlank(key)) {
            String mode = req.getParameter(PARAM_MODE);
            Map<String, String> jsonResponse = marshalNode(nodeRef);
            if ("onlykeys".equals(mode)) {

                LinkedList<String> list = new LinkedList<String>(jsonResponse.keySet());
                JSONValue.writeJSONString(list, res.getWriter());
                return;
            } else {
                JSONValue.writeJSONString(jsonResponse, res.getWriter());
                return;
            }
        } else {
            String val = null;

            Matcher matcher = PAT_PROP.matcher(key);
            if (matcher.matches()) {
                String shortqname = matcher.group(1);
                QName qname = QName.resolveToQName(serviceRegistry.getNamespaceService(), shortqname);
                Map<QName, Serializable> props = serviceRegistry.getNodeService().getProperties(nodeRef);
                if (props.containsKey(qname)) {
                    Serializable value = props.get(qname);
                    if (value instanceof ContentData) {
                        ContentData cdata = (ContentData) value;
                        String detail = matcher.group(2);
                        if (".encoding".equals(detail)) {
                            val = cdata.getEncoding();
                        } else if (".locale".equals(detail)) {
                            val = cdata.getLocale() != null ? cdata.getLocale().toString() : null;
                        } else if (".mimetype".equals(detail)) {
                            val = cdata.getMimetype();
                        } else if (".size".equals(detail)) {
                            val = Long.toString(cdata.getSize());
                        } else if (".contentUrl".equals(detail)) {
                            val = cdata.getContentUrl();
                        } else if (StringUtils.isBlank(detail)) {
                            sendError(res, "content not supported", ERROR_NOTSUP);
                            return;
                            /*
                             * ContentReader reader =
                             * serviceRegistry.getContentService
                             * ().getReader(nodeRef, qname); if(reader != null
                             * && reader.exists()) {
                             * res.setContentType(reader.getMimetype());
                             * res.setContentEncoding(reader.getEncoding());
                             * res.addHeader("Content-Size", "" +
                             * reader.getSize());
                             * reader.getContent(res.getOutputStream()); return;
                             * }
                             */
                        }
                    } else {
                        val = marshalProperty(value);
                    }
                } else {
                    sendError(res, "no such attribute", ERROR_NOATTR);
                    return;
                }
            } else if (key.equals("alf.nodeRef")) {
                val = nodeRef.toString();
            } else if (key.equals("alf.aspects")) {
                val = marshalAspects(nodeRef);
            } else if (key.equals("alf.type")) {
                val = marshalQName(serviceRegistry.getNodeService().getType(nodeRef));
            } else {
                sendError(res, "no such attribute", ERROR_NOATTR);
                return;
            }
            Map<String, String> jsonResponse = new HashMap<String, String>();
            jsonResponse.put("value", val);
            jsonResponse.put("key", key);
            JSONValue.writeJSONString(jsonResponse, res.getWriter());
        }
    }

    @Override
    public void doDelete(WebScriptRequest req, WebScriptResponse res) throws IOException, JSONException {
        res.setContentType(JSON_MIMETYPE);
        res.setContentEncoding(DEFAULT_ENCODING);
        res.setStatus(Status.STATUS_OK);

        String base = req.getParameter(PARAM_BASE);
        String path = req.getParameter(PARAM_PATH);
        String key = req.getParameter(PARAM_KEY);

        if (StringUtils.isBlank(base) || StringUtils.isBlank(path) || StringUtils.isBlank(key)) {
            res.setStatus(Status.STATUS_BAD_REQUEST);
            Map<String, Object> jsonResponse = new HashMap<String, Object>();
            jsonResponse.put("message", "garbage in garbage out");
            jsonResponse.put("errno", "EIO");
            JSONValue.writeJSONString(jsonResponse, res.getWriter());
            return;
        }

        NodeRef nodeRef = locateNode(base, path);
        if (nodeRef == null) {
            res.setStatus(Status.STATUS_NOT_FOUND);
            Map<String, Object> jsonResponse = new HashMap<String, Object>();
            jsonResponse.put("message", "no such file or directory");
            jsonResponse.put("errno", "ENOENT");
            JSONValue.writeJSONString(jsonResponse, res.getWriter());
            return;
        }

        Matcher matcher = PAT_PROP.matcher(key);
        if (matcher.matches()) {
            String shortqname = matcher.group(1);
            String detail = matcher.group(2);
            if (StringUtils.isNotBlank(detail)) {
                sendError(res, "cant remove compound attribute", "ENOTSUP");
                return;
            }
            QName qname = QName.resolveToQName(serviceRegistry.getNamespaceService(), shortqname);
            Map<QName, Serializable> props = serviceRegistry.getNodeService().getProperties(nodeRef);
            if (props.containsKey(qname)) {
                Serializable value = props.get(qname);
                serviceRegistry.getNodeService().removeProperty(nodeRef, qname);
                Map<String, Object> jsonResponse = new HashMap<String, Object>();
                jsonResponse.put("key", key);
                jsonResponse.put("value", marshalProperty(value));
                JSONValue.writeJSONString(jsonResponse, res.getWriter());
                return;
            } else {
                res.setStatus(Status.STATUS_NOT_FOUND);
                Map<String, Object> jsonResponse = new HashMap<String, Object>();
                jsonResponse.put("message", "no such attribute");
                jsonResponse.put("errno", "ENOENT");
                JSONValue.writeJSONString(jsonResponse, res.getWriter());
                return;
            }
        } else {
            sendError(res, "attribute can not be removed", "ENOTSUP");
            return;
        }

    }

    @Override
    public void doPost(WebScriptRequest req, WebScriptResponse res) throws IOException, JSONException {
        res.setContentType(JSON_MIMETYPE);
        res.setContentEncoding(DEFAULT_ENCODING);
        res.setStatus(Status.STATUS_OK);

        String base = req.getParameter(PARAM_BASE);
        String path = req.getParameter(PARAM_PATH);
        String key = req.getParameter(PARAM_KEY);
        String mode = req.getParameter(PARAM_MODE); // create, replace or empty

        if (StringUtils.isBlank(base) || StringUtils.isBlank(path) || StringUtils.isBlank(key)) {
            sendError(res, "garbage in garbage out", ERROR_IO);
            return;
        }

        NodeRef nodeRef = locateNode(base, path);
        if (nodeRef == null) {
            sendError(res, "no such file or directory", ERROR_NOENT, Status.STATUS_NOT_FOUND);
        }

        XAttributeMode xattrmode = XAttributeMode.CREATEORREPLACE;
        if (mode.equals("create")) {
            xattrmode = XAttributeMode.CREATE;
        } else if (mode.equals("replace")) {
            xattrmode = XAttributeMode.REPLACE;
        }

        Matcher matcher = PAT_PROP.matcher(key);
        if (matcher.matches()) {
            // property or compound property
            String shortqname = matcher.group(1);
            String detail = matcher.group(2);
            QName qname = QName.resolveToQName(serviceRegistry.getNamespaceService(), shortqname);

            Map<QName, Serializable> props = serviceRegistry.getNodeService().getProperties(nodeRef);

            if (StringUtils.isNotBlank(detail)) {
                ContentData cdata = (ContentData) props.get(qname);
                String value = req.getContent().getContent();
                if (cdata == null) {
                } else if (".encoding".equals(detail)) {
                    cdata = ContentData.setEncoding(cdata, value);
                } else if (".mimetype".equals(detail)) {
                    cdata = ContentData.setMimetype(cdata, value);
                } else {
                    sendError(res, "attribute not supported", ERROR_NOTSUP);
                    return;
                }
                if (cdata != null) {
                    serviceRegistry.getNodeService().setProperty(nodeRef, qname, cdata);
                }
            } else {
                PropertyDefinition pdef = serviceRegistry.getDictionaryService().getProperty(qname);

                if (xattrmode == XAttributeMode.CREATE) {
                    if (props.containsKey(qname)) {
                        sendError(res, "attribute already exists", ERROR_EXIST);
                        return;
                    }
                } else if (xattrmode == XAttributeMode.REPLACE) {
                    if (!props.containsKey(qname)) {
                        sendError(res, "no such attribute", ERROR_NOATTR);
                        return;
                    }
                }
                DataTypeDefinition ddef = pdef.getDataType();
                Serializable ser = null;
                QName dtype = ddef.getName();
                String value = req.getContent().getContent();
                if (value == null) {
                    // null is fine
                } else if (dtype.equals(DataTypeDefinition.TEXT) || dtype.equals(DataTypeDefinition.MLTEXT)) {
                    // TODO: handle MLTEXT
                    // todo mltext exposure
                    // MLPropertyInterceptor.setMLAware();
                    ser = value;
                } else if (dtype.equals(DataTypeDefinition.BOOLEAN)) {
                    ser = Boolean.parseBoolean(value);
                } else if (dtype.equals(DataTypeDefinition.INT)) {
                    ser = Integer.parseInt(value);
                } else if (dtype.equals(DataTypeDefinition.LONG)) {
                    ser = Long.parseLong(value);
                } else if (dtype.equals(DataTypeDefinition.DOUBLE)) {
                    ser = Double.parseDouble(value);
                } else if (dtype.equals(DataTypeDefinition.FLOAT)) {
                    ser = Float.parseFloat(value);
                } else if (dtype.equals(DataTypeDefinition.QNAME)) {
                    ser = QName.resolveToQName(serviceRegistry.getNamespaceService(), value);
                } else if (dtype.equals(DataTypeDefinition.DATE)) {
                    // cm:created und cm:modified setzen
                    ser = unmarshalDate(value);
                } else if (dtype.equals(DataTypeDefinition.DATETIME)) {
                    ser = unmarshalDateTime(value);
                } else if (dtype.equals(DataTypeDefinition.LOCALE)) {
                    String[] parts = StringUtils.split(value, "_");
                    if (parts.length == 1) {
                        ser = new Locale(parts[0]);
                    } else if (parts.length == 2) {
                        ser = new Locale(parts[0], parts[1]);
                    } else if (parts.length == 3) {
                        ser = new Locale(parts[0], parts[1], parts[2]);
                    } else {
                        throw new RuntimeException("invalid locale");
                    }
                } else {
                    sendError(res, "property type not supported", ERROR_NOTSUP);
                    return;
                }
                boolean auditableRestore = false;
                if (qname.equals(ContentModel.PROP_MODIFIED)) {
                    behaviourFilter.disableBehaviour(nodeRef, ContentModel.ASPECT_AUDITABLE);
                    auditableRestore = true;
                }
                try {
                    serviceRegistry.getNodeService().setProperty(nodeRef, qname, ser);
                } finally {
                    if (auditableRestore) {
                        behaviourFilter.enableBehaviour(nodeRef, ContentModel.ASPECT_AUDITABLE);
                    }
                }
            }

        } else if (key.equals("alf.aspects")) {
            List<QName> aspects = new ArrayList<QName>();
            String value = req.getContent().getContent();
            if (StringUtils.isNotBlank(value)) {
                aspects = unmarshalAspects(value);
            }
            // if create add aspect if not added
            // if replace or createoreplace do repalce
            if (xattrmode == XAttributeMode.CREATE) {
                for (QName fqan : aspects) {
                    serviceRegistry.getNodeService().addAspect(nodeRef, fqan, null);
                }
            } else {
                Set<QName> currentAspects = serviceRegistry.getNodeService().getAspects(nodeRef);
                for (QName fqan : aspects) {
                    if (!currentAspects.contains(fqan)) {
                        serviceRegistry.getNodeService().addAspect(nodeRef, fqan, null);
                    }
                    currentAspects.remove(fqan);
                }
                for (QName fqan : currentAspects) {
                    serviceRegistry.getNodeService().removeAspect(nodeRef, fqan);
                }
            }
        } else if (key.equals("alf.type")) {
            String value = req.getContent().getContent();
            QName type = QName.resolveToQName(serviceRegistry.getNamespaceService(), value);
            serviceRegistry.getNodeService().setType(nodeRef, type);
        } else {
            sendError(res, "garbage in garbage out", ERROR_NOTSUP);
        }

    }

    public BehaviourFilter getBehaviourFilter() {
        return behaviourFilter;
    }

    public void setBehaviourFilter(BehaviourFilter behaviourFilter) {
        this.behaviourFilter = behaviourFilter;
    }
}
