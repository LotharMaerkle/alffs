package de.ecm4u.alfresco.alffs.remote;

import java.io.IOException;
import java.nio.channels.Channels;
import java.nio.channels.FileChannel;
import java.nio.channels.ReadableByteChannel;
import java.util.Date;
import java.util.HashMap;
import java.util.Map;

import org.alfresco.model.ContentModel;
import org.alfresco.repo.policy.BehaviourFilter;
import org.alfresco.repo.transaction.RetryingTransactionHelper.RetryingTransactionCallback;
import org.alfresco.service.cmr.repository.ContentWriter;
import org.alfresco.service.cmr.repository.NodeRef;
import org.apache.commons.lang.StringUtils;
import org.json.JSONException;
import org.json.simple.JSONValue;
import org.springframework.extensions.webscripts.Status;
import org.springframework.extensions.webscripts.WebScriptRequest;
import org.springframework.extensions.webscripts.WebScriptResponse;

public class WriteResource extends AbstractIOResource {
    private BehaviourFilter behaviourFilter;

    @Override
    public void doPut(final WebScriptRequest req, final WebScriptResponse res) throws IOException, JSONException {
        res.setContentType(JSON_MIMETYPE);
        res.setContentEncoding(DEFAULT_ENCODING);
        res.setStatus(Status.STATUS_OK);

        String base = req.getParameter(PARAM_BASE);
        String path = req.getParameter(PARAM_PATH);
        String sizeStr = req.getParameter(PARAM_SIZE);
        String offsetStr = req.getParameter(PARAM_OFFSET);
        final boolean truncate = Boolean.parseBoolean(req.getParameter(PARAM_TRUNCATE));
        final String mtimeStr = req.getParameter(PARAM_MTIME_SEC);

        if (StringUtils.isBlank(base) || StringUtils.isBlank(path) || StringUtils.isBlank(sizeStr) || StringUtils.isBlank(offsetStr)) {
            res.setStatus(Status.STATUS_BAD_REQUEST);
            Map<String, Object> jsonResponse = new HashMap<String, Object>();
            jsonResponse.put("message", "garbage in garbage out");
            JSONValue.writeJSONString(jsonResponse, res.getWriter());
            return;
        }

        final NodeRef nodeRef = locateNode(base, path);
        if (nodeRef == null) {
            res.setStatus(Status.STATUS_NOT_FOUND);
            Map<String, Object> jsonResponse = new HashMap<String, Object>();
            jsonResponse.put("message", "no such file or directory");
            JSONValue.writeJSONString(jsonResponse, res.getWriter());
            return;
        }

        final ContentWriter writer = serviceRegistry.getContentService().getWriter(nodeRef, ContentModel.PROP_CONTENT, true);

        final long size = Long.parseLong(sizeStr);
        final long offset = Long.parseLong(offsetStr);
        serviceRegistry.getTransactionService().getRetryingTransactionHelper().doInTransaction(new RetryingTransactionCallback<Void>() {
            @Override
            public Void execute() throws Throwable {
                try {

                    behaviourFilter.disableBehaviour(nodeRef, ContentModel.ASPECT_AUDITABLE);

                    FileChannel fch = writer.getFileChannel(truncate);
                    Map<String, Object> jsonResponse = new HashMap<String, Object>();
                    ReadableByteChannel wch = null;
                    try {

                        wch = Channels.newChannel(req.getContent().getInputStream());
                        long transfered = fch.transferFrom(wch, offset, size);
                        jsonResponse.put("transfered", Long.toString(transfered));
                    } finally {
                        if (fch != null) {
                            fch.close();
                        }
                        if (wch != null) {
                            wch.close();
                        }
                    }
                    if (StringUtils.isNotBlank(mtimeStr)) {
                        long epochSec = Long.parseLong(mtimeStr);
                        Date modDate = new Date(epochSec * 1000L);
                        serviceRegistry.getNodeService().setProperty(nodeRef, ContentModel.PROP_MODIFIED, modDate);
                    }

                    String curl = writer.getContentUrl();
                    String etag = encodeContentEtag(curl);
                    jsonResponse.put("etag", etag);
                    JSONValue.writeJSONString(jsonResponse, res.getWriter());
                } finally {
                    behaviourFilter.enableBehaviour(nodeRef, ContentModel.ASPECT_AUDITABLE);
                }
                return null;
            }
        }, false, true);

    }

    public BehaviourFilter getBehaviourFilter() {
        return behaviourFilter;
    }

    public void setBehaviourFilter(BehaviourFilter behaviourFilter) {
        this.behaviourFilter = behaviourFilter;
    }

}
