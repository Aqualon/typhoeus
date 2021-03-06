require 'uri'

module Typhoeus
  class Request
    attr_reader   :url
    attr_writer   :headers
    attr_accessor :method, :params, :body, :connect_timeout, :timeout,
                  :user_agent, :response, :cache_timeout, :follow_location,
                  :max_redirects, :proxy, :proxy_username,:proxy_password,
                  :disable_ssl_peer_verification, :disable_ssl_host_verification, :interface,
                  :ssl_cert, :ssl_cert_type, :ssl_key, :ssl_key_type,
                  :ssl_key_password, :ssl_cacert, :ssl_capath, :verbose,
                  :username, :password, :auth_method, :user_agent,
                  :proxy_auth_method, :proxy_type, :attempt_retry, :performed

    alias_method :performed?, :performed

    # Initialize a new Request
    #
    # Options:
    # * +url+ : Endpoint (URL) of the request
    # * +options+   : A hash containing options among :
    # ** +:method+  : :get (default) / :post / :put
    # ** +:params+  : params as a Hash
    # ** +:body+
    # ** +:timeout+ : timeout (ms)
    # ** +:interface+ : interface or ip address (string)
    # ** +:connect_timeout+ : connect timeout (ms)
    # ** +:headers+  : headers as Hash
    # ** +:user_agent+ : user agent (string)
    # ** +:cache_timeout+ : cache timeout (ms)
    # ** +:follow_location
    # ** +:max_redirects
    # ** +:proxy
    # ** +:disable_ssl_peer_verification
    # ** +:disable_ssl_host_verification
    # ** +:ssl_cert
    # ** +:ssl_cert_type
    # ** +:ssl_key
    # ** +:ssl_key_type
    # ** +:ssl_key_password
    # ** +:ssl_cacert
    # ** +:ssl_capath
    # ** +:verbose
    # ** +:username
    # ** +:password
    # ** +:auth_method
    # ** +:retry+ : retry on 503 or 504 (true|false)
    #
    def initialize(url, options = {})
      @method           = options[:method] || :get
      @params           = options[:params]
      @body             = options[:body]
      @timeout          = options[:timeout]
      @connect_timeout  = options[:connect_timeout]
      @interface        = options[:interface]
      @headers          = options[:headers] || {}
      @user_agent       = options[:user_agent] || Typhoeus::USER_AGENT
      @cache_timeout    = options[:cache_timeout]
      @follow_location  = options[:follow_location]
      @max_redirects    = options[:max_redirects]
      @proxy            = options[:proxy]
      @proxy_type       = options[:proxy_type]
      @proxy_username   = options[:proxy_username]
      @proxy_password   = options[:proxy_password]
      @proxy_auth_method = options[:proxy_auth_method]
      @disable_ssl_peer_verification = options[:disable_ssl_peer_verification]
      @disable_ssl_host_verification = options[:disable_ssl_host_verification]
      @ssl_cert         = options[:ssl_cert]
      @ssl_cert_type    = options[:ssl_cert_type]
      @ssl_key          = options[:ssl_key]
      @ssl_key_type     = options[:ssl_key_type]
      @ssl_key_password = options[:ssl_key_password]
      @ssl_cacert       = options[:ssl_cacert]
      @ssl_capath       = options[:ssl_capath]
      @verbose          = options[:verbose]
      @username         = options[:username]
      @password         = options[:password]
      @auth_method      = options[:auth_method]
      @attempt_retry    = !options.has_key?(:retry) || options[:retry]
      @performed        = false

      if @method == :post
        @url = url
      else
        @url = @params ? "#{url}?#{params_string}" : url
      end

      @parsed_uri = URI.parse(@url)

      @on_complete      = nil
      @on_retry         = nil
      @after_complete   = nil
      @handled_response = nil
    end

    LOCALHOST_ALIASES = %w[ localhost 127.0.0.1 0.0.0.0 ]

    def localhost?
      LOCALHOST_ALIASES.include?(@parsed_uri.host)
    end

    def host
      slash_location = @url.index('/', 8)
      if slash_location
        @url.slice(0, slash_location)
      else
        query_string_location = @url.index('?')
        return query_string_location ? @url.slice(0, query_string_location) : @url
      end
    end

    def host_domain
      @parsed_uri.host
    end

    def headers
      @headers["User-Agent"] = @user_agent
      @headers
    end

    def params_string
      traversal = Typhoeus::Utils.traverse_params_hash(params)
      Typhoeus::Utils.traversal_to_param_string(traversal)
    end

    def on_retry(&block)
      @on_retry = block
    end

    def on_retry=(proc)
      @on_retry = proc
    end

    def on_complete(&block)
      @on_complete = block
    end

    def on_complete=(proc)
      @on_complete = proc
    end

    def after_complete(&block)
      @after_complete = block
    end

    def after_complete=(proc)
      @after_complete = proc
    end

    def call_retry_handler(response)
      @on_retry.call(response) if @on_retry
    end

    def call_handlers
      if @on_complete
        @handled_response = @on_complete.call(response)
        call_after_complete
      end
    end

    def call_after_complete
      @after_complete.call(@handled_response) if @after_complete
    end

    def handled_response=(val)
      @handled_response = val
    end

    def handled_response
      @handled_response || response
    end

    def inspect
      result = ":method => #{self.method.inspect},\n" <<
               "\t:url => #{URI.parse(self.url).to_s}"
      if self.body and !self.body.empty?
        result << ",\n\t:body => #{self.body.inspect}"
      end

      if self.params and !self.params.empty?
        result << ",\n\t:params => #{self.params.inspect}"
      end

      if self.headers and !self.headers.empty?
        result << ",\n\t:headers => #{self.headers.inspect}"
      end

      result
    end

    def cache_key
      Digest::SHA1.hexdigest(url)
    end

    def requeued?
      @has_been_requeued
    end

    def mark_requeued
      @has_been_requeued = true
    end

    def retry?
      @attempt_retry
    end

    def self.run(url, params)
      r = new(url, params)
      Typhoeus::Hydra.hydra.queue r
      Typhoeus::Hydra.hydra.run
      r.response
    end

    def self.get(url, params = {})
      run(url, params.merge(:method => :get))
    end

    def self.post(url, params = {})
      run(url, params.merge(:method => :post))
    end

    def self.put(url, params = {})
      run(url, params.merge(:method => :put))
    end

    def self.delete(url, params = {})
      run(url, params.merge(:method => :delete))
    end

    def self.head(url, params = {})
      run(url, params.merge(:method => :head))
    end
  end
end
