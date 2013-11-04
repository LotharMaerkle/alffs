package de.ecm4u.alfresco.alffs.remote;

import java.io.IOException;
import java.io.Serializable;
import java.text.ParseException;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.Date;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.TimeZone;
import java.util.regex.Pattern;

import org.alfresco.model.ContentModel;
import org.alfresco.service.ServiceRegistry;
import org.alfresco.service.cmr.model.FileInfo;
import org.alfresco.service.cmr.model.FileNotFoundException;
import org.alfresco.service.cmr.repository.ChildAssociationRef;
import org.alfresco.service.cmr.repository.ContentData;
import org.alfresco.service.cmr.repository.MLText;
import org.alfresco.service.cmr.repository.NodeRef;
import org.alfresco.service.cmr.repository.StoreRef;
import org.alfresco.service.namespace.QName;
import org.apache.commons.lang.StringUtils;
import org.json.JSONException;
import org.json.simple.JSONValue;
import org.springframework.extensions.webscripts.AbstractWebScript;
import org.springframework.extensions.webscripts.Status;
import org.springframework.extensions.webscripts.WebScriptException;
import org.springframework.extensions.webscripts.WebScriptRequest;
import org.springframework.extensions.webscripts.WebScriptResponse;

public abstract class AbstractIOResource extends AbstractWebScript {
	protected ServiceRegistry serviceRegistry;
	protected final static String JSON_MIMETYPE = "application/json";
	protected final static String BIN_MIMETYPE = "application/json";

	protected final static String DEFAULT_ENCODING = "UTF-8";
	protected final static String PARAM_BASE = "base";
	protected final static String PARAM_PATH = "path";
	protected final static String PARAM_NEWPATH = "newpath";
	protected final static String PARAM_TYPE = "type";
	protected final static String PARAM_SIZE = "size";
	protected final static String PARAM_OFFSET = "offset";
	protected final static String PARAM_TRUNCATE = "truncate";
	protected static final String PARAM_KEY = "key";
	protected static final String PARAM_VALUE = "value";
	protected static final String PARAM_FLAGS = "flags";
	protected static final String PARAM_MODE = "mode";
	protected static final String PARAM_ATIME_SEC = "atime_sec";
	protected static final String PARAM_ATIME_NSEC = "atime_nsec";
	protected static final String PARAM_MTIME_SEC = "mtime_sec";
	protected static final String PARAM_MTIME_NSEC = "mtime_nsec";

	protected static final Pattern PAT_PROP = Pattern
			.compile("^alf\\.prop\\.([^\\.]+)(\\..*)?$");

	// stat stmode flags
	protected static final int S_IFDIR = 0040000;
	protected static final int S_IFREG = 0100000;
	protected static final int S_IFLNK = 0120000;

	// open flags from linux
	protected static final int O_CREAT = 64;
	protected static final int O_WRONLY = 1;
	protected static final int O_RDWR = 2;
	protected static final int O_EXCL = 128;

	protected static final String ISO_TIME = "yyyy-MM-dd'T'HH:mm:ss";
	protected static final String ISO_DATE = "yyyy-MM-dd";

	// dont use numeric values here, instead map by string
	protected static final String ERROR_IO = "EIO";
	protected static final String ERROR_NOENT = "ENOENT";
	protected static final String ERROR_NOTSUP = "ENOTSUP";
	protected static final String ERROR_NOATTR = "ENOATTR";
	protected static final String ERROR_EXIST = "EEXIST";
	protected static final String ERROR_NOTEMPTY = "ENOTEMPTY";
	protected static final String ERROR_NOTDIR = "ENOTDIR";
	protected static final String ERROR_ISDIR = "EISDIR";

	protected static final String HEADER_IF_NONE_MATCH = "If-None-Match";

	protected NodeRef companyHomeRef = null;

	protected boolean isSet(long field, int bit) {
		if ((field & bit) == bit) {
			return true;
		}
		return false;
	}

	@Override
	public void execute(WebScriptRequest req, WebScriptResponse res)
			throws IOException {
		String method = getDescription().getMethod().toUpperCase();
		try {

			if (method.equals("GET")) {
				doGet(req, res);
			} else if (method.equals("POST")) {
				doPost(req, res);
			} else if (method.equals("DELETE")) {
				doDelete(req, res);
			} else if (method.equals("PUT")) {
				doPut(req, res);
			}
		} catch (JSONException e) {
			throw new WebScriptException("json error", e);
		}

	}

	protected void doPut(WebScriptRequest req, WebScriptResponse res)
			throws JSONException, IOException {
	}

	protected void doDelete(WebScriptRequest req, WebScriptResponse res)
			throws JSONException, IOException {
	}

	protected void doPost(WebScriptRequest req, WebScriptResponse res)
			throws JSONException, IOException {
	}

	protected void doGet(WebScriptRequest req, WebScriptResponse res)
			throws JSONException, IOException {
	}

	protected NodeRef locateNode(String base, String path) {
		// for now only company home

		if (!"workspace://SpacesStore/app:company_home".equals(base)) {
			throw new RuntimeException(
					"only company home is currently supported as a base path");
		}
		NodeRef baseRef = getCompanyHome();
		List<String> parts = new ArrayList<String>(Arrays.asList(StringUtils
				.split(path, "/")));

		FileInfo resolveNamePath;
		try {
			resolveNamePath = serviceRegistry.getFileFolderService()
					.resolveNamePath(baseRef, parts, true);
		} catch (FileNotFoundException e) {
			return null;
		}
		if (resolveNamePath != null) {
			return resolveNamePath.getNodeRef();
		}
		return null;
	}

	protected NodeRef resolveNamePathFromCompanyHome(String path) {
		List<String> parts = new ArrayList<String>(Arrays.asList(StringUtils
				.split(path, "/")));

		FileInfo resolveNamePath;
		try {
			resolveNamePath = serviceRegistry.getFileFolderService()
					.resolveNamePath(getCompanyHome(), parts, true);
		} catch (FileNotFoundException e) {
			return null;
		}
		if (resolveNamePath != null) {
			return resolveNamePath.getNodeRef();
		}
		return null;
	}

	protected NodeRef getCompanyHome() {

		if (companyHomeRef != null) {
			return companyHomeRef;
		}

		NodeRef rootNodeRef = serviceRegistry.getNodeService().getRootNode(
				StoreRef.STORE_REF_WORKSPACE_SPACESSTORE);
		List<ChildAssociationRef> childs = serviceRegistry.getNodeService()
				.getChildAssocs(
						rootNodeRef,
						ContentModel.ASSOC_CHILDREN,
						QName.createQName("app:company_home",
								serviceRegistry.getNamespaceService()));
		if (childs.isEmpty()) {
			return null;
		}
		if (childs.size() > 1) {
			throw new RuntimeException("company home noderef not found");
		}
		return childs.get(0).getChildRef();

	}

	protected void sendError(WebScriptResponse response, String message,
			String code) {
		sendError(response, message, code, Status.STATUS_BAD_REQUEST);
	}

	protected void sendError(WebScriptResponse response, String message,
			String code, int httpstatus) {
		response.setStatus(httpstatus);
		Map<String, Object> jsonResponse = new HashMap<String, Object>();
		jsonResponse.put("message", message);
		jsonResponse.put("errno", code);
		try {
			JSONValue.writeJSONString(jsonResponse, response.getWriter());
		} catch (IOException e) {
			throw new RuntimeException(e);
		}
	}

	protected int getNodeFlGags(NodeRef nodeRef) {
		FileInfo finfo = serviceRegistry.getFileFolderService().getFileInfo(
				nodeRef);

		if (finfo.isLink()) {
			return S_IFLNK;
		} else if (finfo.isFolder()) {
			return S_IFDIR;
		}
		return S_IFREG;
	}

	protected String formatIsoUTC(Date timestamp) {
		SimpleDateFormat format = new SimpleDateFormat(ISO_TIME);
		format.getCalendar().setTimeZone(TimeZone.getTimeZone("UTC"));
		return format.format(timestamp);
	}

	protected Map<String, String> marshalNode(NodeRef nodeRef) {
		Map<QName, Serializable> props = serviceRegistry.getNodeService()
				.getProperties(nodeRef);
		Map<String, String> attrs = new HashMap<String, String>(
				props.size() + 5);
		QName type = serviceRegistry.getNodeService().getType(nodeRef);
		for (QName key : props.keySet()) {
			String prefixQName = key.toPrefixString(serviceRegistry
					.getNamespaceService());
			Serializable value = props.get(key);
			if (value instanceof ContentData) {
				marshalContentProperty(key, (ContentData) value, attrs);
			} else {
				String jsonVal = marshalProperty(value);
				attrs.put("alf.prop." + prefixQName, jsonVal);
			}

		}
		attrs.put("alf.aspects", marshalAspects(nodeRef));
		attrs.put("alf.type", marshalQName(type));
		attrs.put("alf.nodeRef", nodeRef.toString());
		return attrs;
	}

	private void marshalMLTextProperty(QName key, MLText mltext,
			Map<String, String> attrs) {
		String prefixQName = key.toPrefixString(serviceRegistry
				.getNamespaceService());

		if (mltext == null) {
			attrs.put("alf.prop." + prefixQName, null);
			return;
		}

		for (Locale loc : mltext.keySet()) {
			String locName = loc.toString();
			attrs.put("alf.prop." + prefixQName + "." + locName,
					mltext.get(loc));
		}
	}

	private void marshalContentProperty(QName key, ContentData value,
			Map<String, String> attrs) {
		String prefixQName = key.toPrefixString(serviceRegistry
				.getNamespaceService());
		if (value == null) {
			attrs.put("alf.prop." + prefixQName, null);
			return;
		}
		// dont add in listings because content will be returned on getxattr
		// attrs.put("alf.prop." + prefixQName, value.toString());
		attrs.put("alf.prop." + prefixQName + ".encoding", value.getEncoding());
		attrs.put("alf.prop." + prefixQName + ".mimetype", value.getMimetype());
		attrs.put("alf.prop." + prefixQName + ".locale",
				value.getLocale() != null ? value.getLocale().toString() : null);
		attrs.put("alf.prop." + prefixQName + ".size",
				Long.toString(value.getSize()));
		attrs.put("alf.prop." + prefixQName + ".contentUrl",
				value.getContentUrl());
	}

	protected String marshalQName(QName qname) {
		return qname.toPrefixString(serviceRegistry.getNamespaceService());
	}

	protected String marshalAspects(NodeRef nodeRef) {
		List<String> aspects = new ArrayList<String>();
		for (QName aspectQName : serviceRegistry.getNodeService().getAspects(
				nodeRef)) {

			aspects.add(aspectQName.toPrefixString(serviceRegistry
					.getNamespaceService()));
		}
		return marshalList(aspects);
	}

	protected List<QName> unmarshalAspects(String aspects) {
		String[] vals = StringUtils.split(aspects, ",");
		List<QName> list = new ArrayList<QName>(vals.length);
		for (String aspect : vals) {
			QName fqan = QName.resolveToQName(
					serviceRegistry.getNamespaceService(), aspect);
			list.add(fqan);
		}
		return list;
	}

	protected String marshalProperty(Serializable value) {
		String strVal = null;
		if (value != null) {
			if (value instanceof Date) {
				Date date = (Date) value;
				strVal = formatIsoUTC(date);
			} else if (value instanceof Collection) {
				strVal = marshalList((Collection) value);
			} else if (value instanceof Number) {
				strVal = value.toString();
			} else {
				strVal = value.toString();
			}
		}
		return strVal;
	}

	protected Date unmarshalDate(String value) {
		SimpleDateFormat sdf = new SimpleDateFormat(ISO_DATE);
		Date date;
		try {
			date = sdf.parse(value);
		} catch (ParseException e) {
			throw new RuntimeException(e);
		}
		return date;
	}

	protected Date unmarshalDateTime(String value) {
		SimpleDateFormat sdf = new SimpleDateFormat(ISO_TIME);
		Date date;
		try {
			date = sdf.parse(value);
		} catch (ParseException e) {
			throw new RuntimeException(e);
		}
		return date;
	}

	protected String marshalList(Collection list) {
		return StringUtils.join(list, ",");
	}

	protected boolean isDirectory(QName type) {
		return serviceRegistry.getDictionaryService().isSubClass(type,
				ContentModel.TYPE_FOLDER);
	}

	protected boolean isFile(QName type) {
		return serviceRegistry.getDictionaryService().isSubClass(type,
				ContentModel.TYPE_CONTENT);
	}

	protected String encodeContentEtag(String url) {
		url = url.replaceFirst("store://", "");
		return url.replaceAll("/", "_");
	}

	public ServiceRegistry getServiceRegistry() {
		return serviceRegistry;
	}

	public void setServiceRegistry(ServiceRegistry serviceRegistry) {
		this.serviceRegistry = serviceRegistry;
	}

}
